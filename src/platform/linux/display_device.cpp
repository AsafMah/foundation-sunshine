// local includes
#include "src/display_device/settings.h"

namespace display_device {

  device_info_map_t
  enum_available_devices() {
    // Not implemented
    return {};
  }

  std::string
  get_display_name(const std::string &value) {
    // Not implemented, but just passthrough the value
    return value;
  }

  device_display_mode_map_t
  get_current_display_modes(const std::unordered_set<std::string> &) {
    // Not implemented
    return {};
  }

  bool
  set_display_modes(const topology::mutation_capability_t &capability, const device_display_mode_map_t &) {
    capability.require(topology::operation_e::write_display_modes);
    // Not implemented
    return false;
  }

  bool
  is_primary_device(const std::string &) {
    // Not implemented
    return false;
  }

  bool
  set_as_primary_device(const topology::mutation_capability_t &capability, const std::string &) {
    capability.require(topology::operation_e::write_primary);
    // Not implemented
    return false;
  }

  hdr_state_map_t
  get_current_hdr_states(const std::unordered_set<std::string> &) {
    // Not implemented
    return {};
  }

  bool
  set_hdr_states(const topology::mutation_capability_t &capability, const hdr_state_map_t &) {
    capability.require(topology::operation_e::write_hdr);
    // Not implemented
    return false;
  }

  active_topology_t
  get_current_topology() {
    // Not implemented
    return {};
  }

  bool
  is_topology_valid(const active_topology_t &topology) {
    // Not implemented
    return false;
  }

  bool
  is_topology_the_same(const active_topology_t &a, const active_topology_t &b) {
    // Not implemented
    return false;
  }

  bool
  set_topology(const topology::mutation_capability_t &capability, const active_topology_t &) {
    capability.require(topology::operation_e::write_topology);
    // Not implemented
    return false;
  }

  struct settings_t::audio_data_t {
    // Not implemented
  };

  struct settings_t::persistent_data_t {
    // Not implemented
  };

  settings_t::settings_t() {
    // Not implemented
  }

  settings_t::~settings_t() {
    // Not implemented
  }

  bool
  settings_t::is_changing_settings_going_to_fail() const {
    // Not implemented
    return false;
  }

  settings_t::apply_result_t
  settings_t::apply_config(const topology::mutation_capability_t &capability, const parsed_config_t &, const rtsp_stream::launch_session_t &,
    automatic_vdd_policy_e, const boost::optional<active_topology_t> &) {
    capability.require(topology::operation_e::configure_display);
    // Not implemented
    return { apply_result_t::result_e::success };
  }

  bool
  settings_t::revert_settings(const topology::mutation_capability_t &capability, revert_reason_e reason, automatic_vdd_policy_e vdd_policy) {
    capability.require(topology::operation_e::restore_state);
    // Not implemented
    (void)reason;  // Unused parameter
    (void)vdd_policy;  // Unused parameter
    return true;
  }

  void
  settings_t::load_persistence(const topology::mutation_capability_t &capability) {
    capability.require(topology::operation_e::restore_state);
  }

  void
  settings_t::reset_persistence(const topology::mutation_capability_t &capability, automatic_vdd_policy_e) {
    capability.require(topology::operation_e::reset_persistence);
    // Not implemented
  }

}  // namespace display_device
