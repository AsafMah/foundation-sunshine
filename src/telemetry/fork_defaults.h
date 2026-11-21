/**
 * @file src/telemetry/fork_defaults.h
 * @brief Fork (AsafMah/foundation-sunshine): the defaults of the two telemetry choices.
 *
 * Upstream sends usage statistics and error reports unless the person turns them off. This fork sends nothing
 * until the person turns a choice on (About > Privacy, or telemetry_consent / telemetry_errors = on in sunshine.conf).
 * An explicit value in sunshine.conf always wins. Everything that decides reads these two constants: the telemetry
 * service (consent_from_config), the crash reporter (crash::errors_enabled_from_conf) and GET /api/config, which
 * reports the default for an unset key so the Web UI and the control panel show the real state.
 * Reverting the fork default is setting both to true.
 */
#pragma once

#include <string>
#include <string_view>

namespace telemetry::fork_defaults {
  inline constexpr bool USAGE_ON = false;
  inline constexpr bool ERRORS_ON = false;

  inline constexpr std::string_view value_of(bool on) { return on ? "on" : "off"; }

  /** The values the telemetry service takes as a choice (service.cpp truthy/falsy); anything else is the default. */
  inline bool recognized(std::string_view v) {
    for (const std::string_view known : { "on", "true", "1", "enabled", "yes", "off", "false", "0", "disabled", "no" })
      if (v == known) return true;
    return false;
  }

  /**
   * GET /api/config: a choice that is unset, or holds a retired or unrecognised value such as 'ask', is reported as what the host does
   * with it, so a page never shows "on" for a choice that is off. foundation-ui takes 'ask' for "not answered" and would hand a stale
   * cached 'on' back to the host, an opt-in nobody gave. An explicit choice is passed on as written.
   */
  template <class Map>
  void fill_unset(Map &vars) {
    const auto usage = vars.find("telemetry_consent");
    const bool usage_old_no = usage != vars.end() && usage->second == "off";
    if (usage == vars.end() || !recognized(usage->second)) vars["telemetry_consent"] = std::string { value_of(USAGE_ON) };
    const auto errors = vars.find("telemetry_errors");
    // an old single answer "off" stays off for error reports (consent_from_config)
    if (errors == vars.end() || !recognized(errors->second)) vars["telemetry_errors"] = std::string { value_of(usage_old_no ? false : ERRORS_ON) };
  }
}  // namespace telemetry::fork_defaults