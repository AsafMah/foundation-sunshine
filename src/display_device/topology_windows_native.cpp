/**
 * @file src/display_device/topology_windows_native.cpp
 * @brief The P1 Windows `native_platform_t` over `native_edge_t`; see the header.
 */
#include "topology_windows_native.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace display_device::topology {

  namespace {
    std::unexpected<error_t>
    fail(error_code_e code, std::string detail) {
      return std::unexpected(error_t { code, std::move(detail), {} });
    }

    int
    hex_value(char c) {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    }

    bool
    is_dash_position(std::size_t index) {
      return index == 9 || index == 14 || index == 19 || index == 24;
    }

    std::string
    describe(const driver_version_t &version) {
      return std::to_string(version.parts[0]) + '.' + std::to_string(version.parts[1]) + '.' +
             std::to_string(version.parts[2]) + '.' + std::to_string(version.parts[3]);
    }

    std::optional<std::string>
    container_gate(const std::optional<std::string> &text) {
      if (!text) return "the virtual display driver version could not be read";
      const auto version = parse_driver_version(*text);
      if (!version) return "the virtual display driver version \"" + *text + "\" is not a four-part version";
      if (auto refusal = container_driver_refusal(*version)) {
        return "the virtual display driver version " + *text + " is not accepted: " + *refusal;
      }
      return std::nullopt;
    }

    std::string
    describe(const edge_mode_t &mode) {
      return std::to_string(mode.width) + 'x' + std::to_string(mode.height) + '@' + std::to_string(mode.refresh_hz);
    }

    /** Within 1 Hz of the whole-Hz rate the driver lists, as the legacy display path compares refresh rates. */
    bool
    same_whole_hz(const display_device::refresh_rate_t &rate, unsigned hz) {
      if (rate.denominator == 0) return false;
      const auto observed = static_cast<long double>(rate.numerator);
      const auto wanted = static_cast<long double>(hz) * rate.denominator;
      return (observed > wanted ? observed - wanted : wanted - observed) <= rate.denominator;
    }

    const observed_display_t *
    head_with(const inventory_t &inventory, const head_guid_t &guid) {
      const observed_display_t *found = nullptr;
      for (const auto &display : inventory.displays) {
        if (display.head && display.head->guid == guid) {
          if (found) return nullptr;
          found = &display;
        }
      }
      return found;
    }

    /** Listed on the desktop with an OS display name and geometry, so it can be captured. */
    bool
    on_desktop(const observed_display_t &display) {
      return display.info.device_state != device_state_e::inactive && display.layout && !display.info.display_name.empty();
    }
  }  // namespace

  std::optional<driver_version_t>
  parse_driver_version(std::string_view text) {
    driver_version_t version;
    std::size_t part = 0;
    std::size_t start = 0;
    while (true) {
      const auto end = text.find('.', start);
      const auto piece = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
      if (part >= version.parts.size() || piece.empty() || piece.size() > 5) return std::nullopt;
      std::uint32_t value = 0;
      for (const auto c : piece) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
      }
      if (value > 65535) return std::nullopt;
      version.parts[part++] = value;
      if (end == std::string_view::npos) break;
      start = end + 1;
    }
    if (part != version.parts.size()) return std::nullopt;
    return version;
  }

  std::optional<std::string>
  container_driver_refusal(const driver_version_t &version) {
    const auto &[major, minor, build, revision] = version.parts;
    if (major == 100) {
      if (version < driver_version_t { { 100, 0, 1, 4 } }) {
        return "it is older than v0.1.4 (100.0.1.4), the first release that reports the head GUID as its ContainerId";
      }
      return std::nullopt;
    }
    if (major == 15 && minor == 0 && build == 15) return std::nullopt;
    return "it is not a tagged ZakoVDD release (" + describe(version) + ")";
  }

  std::string
  format_guid(const uuid_bytes_t &bytes) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string text { "{" };
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      if (i == 4 || i == 6 || i == 8 || i == 10) text.push_back('-');
      text.push_back(digits[bytes[i] >> 4]);
      text.push_back(digits[bytes[i] & 0x0F]);
    }
    text.push_back('}');
    return text;
  }

  std::optional<uuid_bytes_t>
  parse_guid(std::string_view text) {
    if (text.size() != 38 || text.front() != '{' || text.back() != '}') return std::nullopt;
    uuid_bytes_t bytes {};
    std::size_t byte = 0;
    for (std::size_t i = 1; i + 1 < text.size();) {
      if (is_dash_position(i)) {
        if (text[i] != '-') return std::nullopt;
        ++i;
        continue;
      }
      const auto high = hex_value(text[i]);
      const auto low = hex_value(text[i + 1]);
      if (high < 0 || low < 0 || is_dash_position(i + 1)) return std::nullopt;
      bytes[byte++] = static_cast<std::uint8_t>(high << 4 | low);
      i += 2;
    }
    if (byte != bytes.size()) return std::nullopt;
    return bytes;
  }

  windows_native_platform_t::windows_native_platform_t(native_edge_t &edge, windows_native_options_t options):
      edge_(edge),
      options_(std::move(options)) {
    if (!valid_uuid(options_.driver)) throw std::invalid_argument { "the native platform needs a driver instance" };
    if (options_.virtual_name.empty()) throw std::invalid_argument { "the native platform needs the virtual head name" };
    if (options_.poll <= std::chrono::milliseconds::zero()) throw std::invalid_argument { "the native platform needs a positive poll" };
    if (options_.wait < std::chrono::milliseconds::zero()) throw std::invalid_argument { "the native platform needs a non-negative wait" };
  }

  inventory_t
  windows_native_platform_t::inventory() {
    inventory_t inventory;
    inventory.revision = inventory_revision_t { ++revision_ };
    const auto unavailable = [&] {
      inventory_t empty;
      empty.revision = inventory.revision;
      return empty;
    };
    auto snapshot = edge_.snapshot();
    if (!snapshot.available) return unavailable();

    std::set<std::string> seen;
    for (auto &edge_display : snapshot.displays) {
      if (edge_display.id.value.empty() || !seen.insert(edge_display.id.value).second) return unavailable();
      if (edge_display.kind == head_class_e::unknown) return unavailable();
      const bool driver_head = edge_display.kind == head_class_e::driver_head;
      if (driver_head != (edge_display.info.friendly_name == options_.virtual_name)) return unavailable();

      observed_display_t display;
      display.id = edge_display.id;
      display.info = std::move(edge_display.info);
      display.layout = edge_display.layout;
      display.adapter = edge_display.adapter;
      display.incarnation = source_incarnation_t { edge_display.arrival };

      if (driver_head && edge_display.container && *edge_display.container != uuid_bytes_t {}) {
        if (edge_display.arrival == 0) return unavailable();
        const head_identity_t identity { options_.driver, head_guid_t { *edge_display.container }, head_incarnation_t { edge_display.arrival } };
        display.head = identity;
        inventory.heads.push_back({ identity, std::nullopt, edge_display.adapter.value_or(adapter_luid_t {}), 0, display.id });
      }
      inventory.displays.push_back(std::move(display));
    }
    inventory.available = true;
    return inventory;
  }

  bool
  windows_native_platform_t::is_virtual_head(const observed_display_t &display) const {
    return display.info.friendly_name == options_.virtual_name;
  }

  bool
  windows_native_platform_t::wait_for(const std::function<bool(const inventory_t &)> &done) {
    const auto start = edge_.now();
    while (true) {
      const auto observed = inventory();
      if (observed.available && done(observed)) return true;
      if (edge_.now() - start >= options_.wait) return false;
      edge_.sleep(options_.poll);
    }
  }

  result_t<creation_delivery_e>
  windows_native_platform_t::do_create_head(const mutation_capability_t &capability, const head_guid_t &intent) {
    const version_gate_t gate = container_gate;

    const auto sent = edge_.send_create(capability, intent, gate);
    switch (sent.delivery) {
      case edge_delivery_e::not_sent:
        return fail(error_code_e::resource_unavailable, "virtual display creation was not sent: " + sent.detail);
      case edge_delivery_e::uncertain:
        return creation_delivery_e::uncertain;
      case edge_delivery_e::delivered:
        break;
    }
    wait_for([&](const inventory_t &observed) {
      return std::ranges::any_of(observed.heads, [&](const observed_head_t &head) { return head.identity.guid == intent; });
    });
    return creation_delivery_e::accepted;
  }

  result_t<void>
  windows_native_platform_t::do_remove_all_heads(const mutation_capability_t &capability) {
    const auto sent = edge_.send_destroy_all(capability);
    if (sent.delivery != edge_delivery_e::delivered) {
      return fail(error_code_e::unknown_applied_state, "virtual display removal was not completed by the driver: " + sent.detail);
    }
    const bool cleared = wait_for([&](const inventory_t &observed) {
      return std::ranges::none_of(observed.displays, [&](const observed_display_t &display) { return is_virtual_head(display); });
    });
    if (!cleared) return fail(error_code_e::unknown_applied_state, "a virtual display head is still listed after removal");
    return {};
  }

  result_t<void>
  windows_native_platform_t::do_apply_fields(const mutation_capability_t &, const os_display_id_t &, const managed_values_t &) {
    return fail(error_code_e::unsupported_mode, "P1 does not write display fields");
  }

  result_t<void>
  windows_native_platform_t::do_change_head_mode(const mutation_capability_t &capability, const observed_display_t &head, const display_mode_t &mode) {
    if (!head.head) return fail(error_code_e::missing_identity, "the display is not a virtual display head");
    if (mode.resolution.width == 0 || mode.resolution.height == 0 || mode.refresh_rate.denominator == 0) {
      return fail(error_code_e::unsupported_mode, "the requested mode is empty");
    }
    const auto guid = head.head->guid;
    const edge_mode_t wanted {
      mode.resolution.width,
      mode.resolution.height,
      (mode.refresh_rate.numerator + mode.refresh_rate.denominator / 2) / mode.refresh_rate.denominator,
    };
    const auto lists = [&](const std::optional<std::vector<edge_mode_t>> &modes) {
      return modes && std::ranges::find(*modes, wanted) != modes->end();
    };

    const auto offered = edge_.list_modes(head.info.display_name);
    if (!offered) return fail(error_code_e::resource_unavailable, "the virtual display's modes could not be read");
    if (!lists(offered)) {
      if (offered->size() >= max_driver_modes) {
        return fail(error_code_e::resource_unavailable, "the virtual display mode list is full, so " + describe(wanted) + " was not added");
      }
      std::vector<edge_mode_t> plan { wanted };
      for (const auto &existing : *offered) {
        if (std::ranges::find(plan, existing) == plan.end()) plan.push_back(existing);
      }
      const auto sent = edge_.send_set_modes(capability, plan, container_gate);
      switch (sent.delivery) {
        case edge_delivery_e::not_sent:
          return fail(error_code_e::resource_unavailable, "the virtual display mode list was not sent: " + sent.detail);
        case edge_delivery_e::uncertain:
          return fail(error_code_e::unknown_applied_state, "the virtual display mode list may have been applied: " + sent.detail);
        case edge_delivery_e::delivered:
          break;
      }
      const bool listed = wait_for([&](const inventory_t &observed) {
        const auto *now = head_with(observed, guid);
        return now && (!on_desktop(*now) || lists(edge_.list_modes(now->info.display_name)));
      });
      if (!listed) return fail(error_code_e::unknown_applied_state, "the virtual display does not offer " + describe(wanted) + " after the mode list changed");
      if (auto active = ensure_active(capability, guid); !active) return active;
      const bool offered_now = wait_for([&](const inventory_t &observed) {
        const auto *now = head_with(observed, guid);
        return now && on_desktop(*now) && lists(edge_.list_modes(now->info.display_name));
      });
      if (!offered_now) return fail(error_code_e::unknown_applied_state, "the virtual display does not offer " + describe(wanted) + " after the mode list changed");
    }

    const auto listed_now = inventory();
    const auto *target = head_with(listed_now, guid);
    if (!target) return fail(error_code_e::unknown_applied_state, "the virtual display is no longer listed");
    const auto set = edge_.set_mode(capability, target->id, mode);
    if (set.delivery == edge_delivery_e::not_sent) {
      return fail(error_code_e::resource_unavailable, "the virtual display mode was not sent: " + set.detail);
    }
    if (set.delivery != edge_delivery_e::delivered) {
      return fail(error_code_e::unknown_applied_state, "the virtual display mode was not applied: " + set.detail);
    }
    const bool applied = wait_for([&](const inventory_t &observed) {
      const auto *now = head_with(observed, guid);
      return now && now->layout && now->layout->mode.resolution.width == wanted.width && now->layout->mode.resolution.height == wanted.height &&
             same_whole_hz(now->layout->mode.refresh_rate, wanted.refresh_hz);
    });
    if (!applied) return fail(error_code_e::unknown_applied_state, "the virtual display does not show " + describe(wanted) + " after the mode was applied");
    return {};
  }

  result_t<void>
  windows_native_platform_t::do_activate_head(const mutation_capability_t &capability, const observed_display_t &head) {
    if (!head.head) return fail(error_code_e::missing_identity, "the display is not a virtual display head");
    return ensure_active(capability, head.head->guid);
  }

  result_t<void>
  windows_native_platform_t::ensure_active(const mutation_capability_t &capability, const head_guid_t &guid) {
    const auto listed = inventory();
    const auto *head = listed.available ? head_with(listed, guid) : nullptr;
    if (!head) return fail(error_code_e::unknown_applied_state, "the virtual display is no longer listed");
    if (on_desktop(*head)) return {};
    const auto sent = edge_.activate(capability, head->id);
    switch (sent.delivery) {
      case edge_delivery_e::not_sent:
        return fail(error_code_e::resource_unavailable, "the virtual display was not added to the desktop: " + sent.detail);
      case edge_delivery_e::uncertain:
        return fail(error_code_e::unknown_applied_state, "the virtual display may have been added to the desktop: " + sent.detail);
      case edge_delivery_e::delivered:
        break;
    }
    const bool active = wait_for([&](const inventory_t &observed) {
      const auto *now = head_with(observed, guid);
      return now && on_desktop(*now);
    });
    if (!active) return fail(error_code_e::unknown_applied_state, "the virtual display is not on the desktop after it was added");
    return {};
  }

  result_t<void>
  windows_native_platform_t::do_arrange(const mutation_capability_t &capability, const std::vector<placement_t> &placements,
    const std::vector<os_display_id_t> &off) {
    if (placements.empty() && off.empty()) return {};
    const auto sent = edge_.arrange(capability, placements, off);
    switch (sent.delivery) {
      case edge_delivery_e::not_sent:
        return fail(error_code_e::resource_unavailable, "the displays were not placed: " + sent.detail);
      case edge_delivery_e::uncertain:
        return fail(error_code_e::unknown_applied_state, "the displays may have been placed: " + sent.detail);
      case edge_delivery_e::delivered:
        break;
    }
    const bool placed = wait_for([&](const inventory_t &observed) {
      const bool all_off = std::none_of(off.begin(), off.end(), [&](const os_display_id_t &id) {
        return std::any_of(observed.displays.begin(), observed.displays.end(), [&](const observed_display_t &display) {
          return display.id == id && on_desktop(display);
        });
      });
      return all_off && std::all_of(placements.begin(), placements.end(), [&](const placement_t &placement) {
        const auto matches = std::count_if(observed.displays.begin(), observed.displays.end(), [&](const observed_display_t &display) {
          return display.id == placement.display;
        });
        const auto it = std::find_if(observed.displays.begin(), observed.displays.end(), [&](const observed_display_t &display) {
          return display.id == placement.display;
        });
        return matches == 1 && on_desktop(*it) && it->layout->position == placement.position;
      });
    });
    if (!placed) return fail(error_code_e::unknown_applied_state, "the displays are not at their places after they were placed");
    return {};
  }
}  // namespace display_device::topology
