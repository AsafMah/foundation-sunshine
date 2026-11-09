/**
 * @file src/display_device/topology_native.cpp
 * @brief P1 managed (native) display path: profile, executor and recovery.
 */
#include "topology_native.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace display_device::topology {

  namespace {
    using json = nlohmann::json;

    constexpr std::size_t max_profile_bytes = 4096;

    std::unexpected<error_t>
    profile_error(error_code_e code, const std::string &detail) {
      return std::unexpected(error_t { code, "display_topology_profile: " + detail, {} });
    }

    std::optional<std::string>
    unknown_key(const json &object, std::initializer_list<std::string_view> allowed) {
      for (const auto &item : object.items()) {
        if (std::find(allowed.begin(), allowed.end(), item.key()) == allowed.end()) {
          return item.key();
        }
      }
      return std::nullopt;
    }

    enum class field_e : std::uint8_t {
      position,
      mode,
      rotation,
      scale,
      hdr,
      primary,
    };

    constexpr std::array all_fields { field_e::position, field_e::mode, field_e::rotation,
      field_e::scale, field_e::hdr, field_e::primary };

    std::string
    field_name(field_e field) {
      switch (field) {
        case field_e::position:
          return "position";
        case field_e::mode:
          return "mode";
        case field_e::rotation:
          return "rotation";
        case field_e::scale:
          return "scale";
        case field_e::hdr:
          return "HDR";
        case field_e::primary:
          return "primary";
      }
      return "field";
    }

    void
    copy_field(managed_values_t &to, const managed_values_t &from, field_e field) {
      switch (field) {
        case field_e::position:
          to.position = from.position;
          break;
        case field_e::mode:
          to.mode = from.mode;
          break;
        case field_e::rotation:
          to.rotation = from.rotation;
          break;
        case field_e::scale:
          to.scale_percent = from.scale_percent;
          break;
        case field_e::hdr:
          to.hdr = from.hdr;
          break;
        case field_e::primary:
          to.primary = from.primary;
          break;
      }
    }

    managed_values_t
    only(const managed_values_t &values, field_e field) {
      managed_values_t result;
      copy_field(result, values, field);
      return result;
    }

    bool
    has(const managed_values_t &values, field_e field) {
      return only(values, field) != managed_values_t {};
    }

    /** Field equality as the journal defines it (`managed_values_t::operator==`). */
    bool
    same(const managed_values_t &a, const managed_values_t &b, field_e field) {
      return only(a, field) == only(b, field);
    }

    /** The values a display currently holds, derived exactly as journal validation derives baselines. */
    managed_values_t
    current_values(const observed_display_t &display) {
      const auto &layout = *display.layout;
      return { layout.position, layout.mode, layout.rotation, layout.scale_percent,
        layout.hdr == hdr_state_e::unknown ? std::nullopt : std::optional { layout.hdr },
        display.info.device_state == device_state_e::primary };
    }

    /** The one display with `id`; null when it is missing or ambiguous. */
    const observed_display_t *
    find_unique(const inventory_t &inventory, const os_display_id_t &id) {
      const observed_display_t *found = nullptr;
      for (const auto &display : inventory.displays) {
        if (display.id != id) {
          continue;
        }
        if (found) {
          return nullptr;
        }
        found = &display;
      }
      return found;
    }

    std::vector<const observed_display_t *>
    virtual_heads(const native_platform_t &platform, const inventory_t &inventory) {
      std::vector<const observed_display_t *> heads;
      for (const auto &display : inventory.displays) {
        if (platform.is_virtual_head(display)) {
          heads.push_back(&display);
        }
      }
      return heads;
    }

    bool
    same_listed_head(const observed_display_t &head, const observed_display_t &expected) {
      return head.id == expected.id && head.adapter == expected.adapter && head.incarnation == expected.incarnation && head.head == expected.head;
    }

    /** Each of `expected` is listed exactly once, with the same ID, adapter, incarnation and head identity. */
    bool
    lists_each(const native_platform_t &platform, const inventory_t &inventory, const std::vector<observed_display_t> &expected) {
      if (!inventory.available) {
        return false;
      }
      const auto heads = virtual_heads(platform, inventory);
      return std::all_of(expected.begin(), expected.end(), [&](const observed_display_t &want) {
        return std::count_if(heads.begin(), heads.end(), [&](const observed_display_t *head) { return same_listed_head(*head, want); }) == 1;
      });
    }

    /** The listed virtual heads are exactly `expected`, and nothing else; `expected` names each display once. */
    bool
    exact_head_set(const native_platform_t &platform, const inventory_t &inventory, const std::vector<observed_display_t> &expected) {
      for (auto it = expected.begin(); it != expected.end(); ++it) {
        if (std::any_of(std::next(it), expected.end(), [&](const observed_display_t &other) { return other.id == it->id; })) {
          return false;
        }
      }
      return lists_each(platform, inventory, expected) && virtual_heads(platform, inventory).size() == expected.size();
    }

    bool
    sole_matching_head(const native_platform_t &platform, const inventory_t &inventory, const observed_display_t &expected) {
      return exact_head_set(platform, inventory, { expected });
    }

    /**
     * Same-run proof when the baseline had no virtual display: every listed head carries
     * this driver, a live incarnation and one of this run's request GUIDs, each at most once.
     */
    bool
    only_requested_heads(const native_platform_t &platform, const inventory_t &inventory, const driver_instance_id_t &driver,
      const std::vector<head_guid_t> &requests) {
      if (!inventory.available) {
        return false;
      }
      std::vector<head_guid_t> seen;
      for (const auto *head : virtual_heads(platform, inventory)) {
        if (!head->head || head->head->driver != driver || head->head->incarnation.value == 0 ||
            std::find(requests.begin(), requests.end(), head->head->guid) == requests.end() ||
            std::find(seen.begin(), seen.end(), head->head->guid) != seen.end()) {
          return false;
        }
        seen.push_back(head->head->guid);
      }
      return true;
    }

    using signature_t = std::vector<std::pair<std::string, device_state_e>>;

    /** Listed on the desktop with an OS display name and geometry, so it can be captured. */
    bool
    on_desktop(const observed_display_t &display) {
      return display.info.device_state != device_state_e::inactive && display.layout && !display.info.display_name.empty();
    }

    /**
     * Displays the transaction neither manages nor created, by identity and state.
     * Geometry is not compared: P1 never writes these displays, it only checks that
     * none appeared, disappeared or changed state around its own writes.
     */
    signature_t
    unmanaged_signature(const native_platform_t &platform, const inventory_t &inventory, const journal_record_t &record) {
      signature_t signature;
      for (const auto &display : inventory.displays) {
        const bool managed = std::any_of(record.managed.begin(), record.managed.end(),
          [&](const managed_display_t &entry) { return entry.source.display == display.id; });
        if (!managed && !platform.is_virtual_head(display)) {
          signature.emplace_back(display.id.value, display.info.device_state);
        }
      }
      std::sort(signature.begin(), signature.end());
      return signature;
    }

    bool
    unmanaged(const native_platform_t &platform, const journal_record_t &record, const observed_display_t &display) {
      return !platform.is_virtual_head(display) && std::none_of(record.managed.begin(), record.managed.end(),
        [&](const managed_display_t &entry) { return entry.source.display == display.id; });
    }

    /** Same mode, rotation, scale and HDR; the corner may differ. */
    bool
    same_shape(layout_t a, const layout_t &b) {
      a.position = b.position;
      return a == b;
    }

    /**
     * Where every display must be around new virtual heads: each unmanaged display on the
     * baseline desktop at its baseline corner, then `heads` in order, left to right from the
     * right edge of those displays, on the primary's row. Windows may instead give an arriving
     * head the origin and the primary, moving the rest (seen with a sole virtual primary).
     * nullopt when a head is not on the desktop in `current`.
     */
    std::optional<std::vector<placement_t>>
    placement_plan(const native_platform_t &platform, const journal_record_t &record, const inventory_t &current,
      const std::vector<os_display_id_t> &heads) {
      std::vector<placement_t> plan;
      std::int32_t edge = 0;
      for (const auto &display : record.baseline.displays) {
        if (!unmanaged(platform, record, display) || display.info.device_state == device_state_e::inactive || !display.layout) {
          continue;
        }
        plan.push_back({ display.id, display.layout->position });
        edge = std::max(edge, display.layout->position.x + static_cast<std::int32_t>(display.layout->mode.resolution.width));
      }
      for (const auto &id : heads) {
        const auto *head = find_unique(current, id);
        if (!head || !on_desktop(*head)) {
          return std::nullopt;
        }
        plan.push_back({ id, { edge, 0 } });
        edge += static_cast<std::int32_t>(head->layout->mode.resolution.width);
      }
      return plan;
    }

    /** Anything not where `plan` puts it, a baseline display reshaped, or unmanaged displays changed in identity or state. */
    bool
    misplaced(const native_platform_t &platform, const inventory_t &current, const journal_record_t &record, const std::vector<placement_t> &plan) {
      if (unmanaged_signature(platform, current, record) != unmanaged_signature(platform, record.baseline, record)) {
        return true;
      }
      return std::any_of(plan.begin(), plan.end(), [&](const placement_t &placement) {
        const auto *now = find_unique(current, placement.display);
        if (!now || !now->layout || now->info.device_state == device_state_e::inactive || now->layout->position != placement.position) {
          return true;
        }
        const auto *before = find_unique(record.baseline, placement.display);
        return before && before->layout && !same_shape(*before->layout, *now->layout);
      });
    }

    /** Off the desktop: listed but inactive. A display that is on but whose layout cannot be read is not "off". */
    bool
    is_off_desktop(const observed_display_t *display) {
      return !display || display->info.device_state == device_state_e::inactive;
    }

    /** On the desktop without a readable layout: nothing about it can be compared or put back. */
    bool
    unreadable(const observed_display_t *display) {
      return display && display->info.device_state != device_state_e::inactive && !display->layout;
    }

    /**
     * Only corners and the primary moved: the same unmanaged displays, the same ones off the
     * desktop, and every mode, rotation and HDR state as in the baseline. Only this drift is
     * put back; anything else is a conflict.
     */
    bool
    movable(const native_platform_t &platform, const inventory_t &current, const journal_record_t &record) {
      std::vector<std::string> ids_now;
      std::vector<std::string> ids_before;
      for (const auto &display : current.displays) {
        if (unmanaged(platform, record, display)) ids_now.push_back(display.id.value);
      }
      for (const auto &display : record.baseline.displays) {
        if (unmanaged(platform, record, display)) ids_before.push_back(display.id.value);
      }
      std::sort(ids_now.begin(), ids_now.end());
      std::sort(ids_before.begin(), ids_before.end());
      if (ids_now != ids_before || std::adjacent_find(ids_now.begin(), ids_now.end()) != ids_now.end()) {
        return false;
      }
      return std::all_of(record.baseline.displays.begin(), record.baseline.displays.end(), [&](const observed_display_t &before) {
        if (!unmanaged(platform, record, before)) return true;
        const auto *now = find_unique(current, before.id);
        if (unreadable(&before) || unreadable(now)) return false;
        const bool was_off = is_off_desktop(&before);
        const bool is_off = is_off_desktop(now);
        if (was_off || is_off) return was_off == is_off;
        return same_shape(*before.layout, *now->layout);
      });
    }

    /**
     * The displays that were off the desktop at baseline and are on it now, when that and corner or
     * primary drift is all that changed: the same unmanaged displays, every display on at baseline still
     * on with its baseline mode, rotation and HDR. nullopt for any other change (a conflict).
     */
    std::optional<std::vector<os_display_id_t>>
    turned_on(const native_platform_t &platform, const inventory_t &current, const journal_record_t &record) {
      std::vector<std::string> ids_now;
      std::vector<std::string> ids_before;
      for (const auto &display : current.displays) {
        if (unmanaged(platform, record, display)) ids_now.push_back(display.id.value);
      }
      for (const auto &display : record.baseline.displays) {
        if (unmanaged(platform, record, display)) ids_before.push_back(display.id.value);
      }
      std::sort(ids_now.begin(), ids_now.end());
      std::sort(ids_before.begin(), ids_before.end());
      if (ids_now != ids_before || std::adjacent_find(ids_now.begin(), ids_now.end()) != ids_now.end()) {
        return std::nullopt;
      }
      std::vector<os_display_id_t> on;
      for (const auto &before : record.baseline.displays) {
        if (!unmanaged(platform, record, before)) continue;
        const auto *now = find_unique(current, before.id);
        // An active display whose layout cannot be read is never "off": a conflict, not something to turn off.
        if (unreadable(&before) || unreadable(now)) return std::nullopt;
        const bool was_off = is_off_desktop(&before);
        const bool is_off = is_off_desktop(now);
        if (was_off) {
          if (!is_off) on.push_back(before.id);
          continue;
        }
        if (is_off || !same_shape(*before.layout, *now->layout)) return std::nullopt;
      }
      return on;
    }

    std::string
    state_text(device_state_e state) {
      switch (state) {
        case device_state_e::inactive: return "off";
        case device_state_e::active: return "on";
        case device_state_e::primary: return "primary";
      }
      return "?";
    }

    std::string
    hdr_text(hdr_state_e hdr) {
      switch (hdr) {
        case hdr_state_e::enabled: return "on";
        case hdr_state_e::disabled: return "off";
        case hdr_state_e::unknown: return "unknown";
      }
      return "?";
    }

    std::string
    mode_text(const display_device::display_mode_t &mode) {
      const auto &rate = mode.refresh_rate;
      std::string hz;
      if (rate.denominator == 0) {
        hz = "?";
      }
      else if (rate.numerator % rate.denominator == 0) {
        hz = std::to_string(rate.numerator / rate.denominator);
      }
      else {
        const auto hundredths = static_cast<unsigned long long>(rate.numerator) * 100ULL / rate.denominator;
        hz = std::to_string(hundredths / 100) + "." + (hundredths % 100 < 10 ? "0" : "") + std::to_string(hundredths % 100);
      }
      return std::to_string(mode.resolution.width) + "x" + std::to_string(mode.resolution.height) + "@" + hz + "Hz";
    }

    std::string
    display_name(const observed_display_t &display) {
      return display.info.friendly_name.empty() ? display.id.value : display.info.friendly_name + " (" + display.id.value + ")";
    }

    /**
     * Every field of every display this transaction does not manage that differs between the
     * baseline and `current`: disappeared, appeared, on/off/primary, corner, mode, rotation, scale
     * and HDR. Empty when nothing differs. Part of every "changed other displays" refusal, so a
     * hardware run shows exactly what Windows did.
     */
    std::string
    other_display_changes(const native_platform_t &platform, const inventory_t &current, const journal_record_t &record) {
      std::vector<std::string> parts;
      for (const auto &before : record.baseline.displays) {
        if (!unmanaged(platform, record, before)) continue;
        const auto *now = find_unique(current, before.id);
        if (!now) {
          parts.push_back(display_name(before) + ": disappeared");
          continue;
        }
        std::vector<std::string> fields;
        if (before.info.device_state != now->info.device_state) {
          fields.push_back(state_text(before.info.device_state) + " -> " + state_text(now->info.device_state));
        }
        if (before.layout && now->layout) {
          const auto &a = *before.layout;
          const auto &b = *now->layout;
          if (a.position != b.position) {
            fields.push_back("corner " + std::to_string(a.position.x) + "," + std::to_string(a.position.y) + " -> " +
                             std::to_string(b.position.x) + "," + std::to_string(b.position.y));
          }
          const bool same_mode = a.mode.resolution.width == b.mode.resolution.width && a.mode.resolution.height == b.mode.resolution.height &&
                                 a.mode.refresh_rate.numerator == b.mode.refresh_rate.numerator &&
                                 a.mode.refresh_rate.denominator == b.mode.refresh_rate.denominator;
          if (!same_mode) {
            // The rounded text can hide a different rate: then the raw numerator/denominator are shown too.
            const bool same_text = mode_text(a.mode) == mode_text(b.mode);
            const auto raw = [&](const display_device::display_mode_t &m) {
              return same_text ? " (" + std::to_string(m.refresh_rate.numerator) + "/" + std::to_string(m.refresh_rate.denominator) + ")" : std::string {};
            };
            fields.push_back("mode " + mode_text(a.mode) + raw(a.mode) + " -> " + mode_text(b.mode) + raw(b.mode));
          }
          if (a.rotation != b.rotation) {
            fields.push_back("rotation " + std::to_string(static_cast<int>(a.rotation)) + " -> " + std::to_string(static_cast<int>(b.rotation)));
          }
          if (a.scale_percent != b.scale_percent) {
            const auto scale = [](const std::optional<std::uint32_t> &value) { return value ? std::to_string(*value) + "%" : std::string { "?" }; };
            fields.push_back("scale " + scale(a.scale_percent) + " -> " + scale(b.scale_percent));
          }
          if (a.hdr != b.hdr) fields.push_back("HDR " + hdr_text(a.hdr) + " -> " + hdr_text(b.hdr));
        }
        if (unreadable(&before)) fields.push_back("on, layout unreadable at baseline");
        if (unreadable(now)) fields.push_back("on, layout unreadable");
        if (now->layout && !before.layout) {
          fields.push_back("now at " + std::to_string(now->layout->position.x) + "," + std::to_string(now->layout->position.y) + " " + mode_text(now->layout->mode));
        }
        if (!fields.empty()) {
          std::string joined;
          for (const auto &field : fields) joined += (joined.empty() ? "" : ", ") + field;
          parts.push_back(display_name(before) + ": " + joined);
        }
      }
      for (const auto &display : current.displays) {
        if (unmanaged(platform, record, display) && !find_unique(record.baseline, display.id)) {
          parts.push_back(display_name(display) + ": appeared (" + state_text(display.info.device_state) + ")");
        }
      }
      std::string text;
      for (const auto &part : parts) text += (text.empty() ? "" : "; ") + part;
      return text;
    }

    /** `what`, followed by exactly what differs on the other displays. */
    std::string
    with_changes(const std::string &what, const native_platform_t &platform, const inventory_t &current, const journal_record_t &record) {
      const auto changes = other_display_changes(platform, current, record);
      return changes.empty() ? what : what + " [" + changes + "]";
    }

    /**
     * Puts unmanaged displays back at their baseline corners and `heads` right of them, in
     * one desktop write, when only corners or the primary drifted. With `restore_active_set`
     * (only while preparing, where every virtual head is this run's) displays that were off at
     * baseline and that Windows turned on are taken off the desktop in the same write. nullopt
     * when everything is in place (`observed` is then the latest inventory); otherwise why it
     * failed, starting with `what`. The write is not journaled: a recovery that finds the same
     * drift puts it back.
     */
    std::optional<std::string>
    place(native_platform_t &platform, const mutation_capability_t &capability, const journal_record_t &record,
      const std::vector<os_display_id_t> &heads, inventory_t &observed, const std::function<bool(const inventory_t &)> &proven,
      const std::string &what, bool restore_active_set) {
      const auto plan = placement_plan(platform, record, observed, heads);
      if (!plan) {
        return what + "; a virtual display is not on the desktop";
      }
      if (!misplaced(platform, observed, record, *plan)) {
        return std::nullopt;
      }
      std::vector<os_display_id_t> off;
      if (restore_active_set) {
        const auto on = turned_on(platform, observed, record);
        if (!on) {
          return with_changes(what, platform, observed, record);
        }
        off = *on;
      }
      else if (!movable(platform, observed, record)) {
        return with_changes(what, platform, observed, record);
      }
      if (const auto arranged = platform.arrange(capability, *plan, off); !arranged) {
        return what + "; placing the displays failed: " + arranged.error().detail;
      }
      observed = platform.inventory();
      if (!observed.available || !proven(observed)) {
        return what + "; the virtual displays changed while the displays were placed";
      }
      if (misplaced(platform, observed, record, *plan)) {
        return with_changes(what + "; placing the displays did not restore them", platform, observed, record);
      }
      return std::nullopt;
    }

    /** Whether a step of `operation` (for `display`, when given) went past planning. */
    bool
    attempted(const journal_record_t &record, journal_operation_e operation, const os_display_id_t *display) {
      return std::any_of(record.steps.begin(), record.steps.end(), [&](const journal_step_t &step) {
        return step.operation == operation && step.state != journal_step_state_e::planned &&
               (!display || step.display == *display);
      });
    }

    const journal_step_t *
    observed_create(const journal_record_t &record) {
      const auto it = std::find_if(record.steps.rbegin(), record.steps.rend(), [](const journal_step_t &step) {
        return step.operation == journal_operation_e::create_head && step.state == journal_step_state_e::observed;
      });
      return it == record.steps.rend() ? nullptr : &*it;
    }

    /** Each owned head as its observed creation listed it, in creation order; nullopt unless every one is named exactly. */
    std::optional<std::vector<observed_display_t>>
    owned_as_observed(const journal_record_t &record) {
      std::vector<observed_display_t> heads;
      for (const auto &step : record.steps) {
        if (step.operation != journal_operation_e::create_head || step.state != journal_step_state_e::observed) {
          continue;
        }
        const auto *head = step.display && step.observed ? find_unique(*step.observed, *step.display) : nullptr;
        // Each observed creation must list the head its own ownership entry names, in order.
        if (!head || !head->head || heads.size() >= record.owned_heads.size() ||
            head->head->guid != record.owned_heads[heads.size()].identity.guid ||
            head->head->driver != record.owned_heads[heads.size()].identity.driver) {
          return std::nullopt;
        }
        heads.push_back(*head);
      }
      if (heads.empty() || heads.size() != record.owned_heads.size()) {
        return std::nullopt;
      }
      return heads;
    }

    /**
     * Relations the journal schema does not check but reconciliation relies on. A journal
     * that breaks one cannot be reconciled safely, so it is In conflict.
     */
    std::optional<std::string>
    semantic_problem(const journal_record_t &record) {
      for (const auto &step : record.steps) {
        switch (step.operation) {
          case journal_operation_e::apply_fields:
            if (!step.display || std::none_of(record.managed.begin(), record.managed.end(), [&](const managed_display_t &entry) {
                  return entry.source.display == *step.display;
                })) {
              return "a field step does not name a managed display";
            }
            break;
          case journal_operation_e::create_head:
            if (!step.head_intent) {
              return "a virtual display step has no request identity";
            }
            if (step.state == journal_step_state_e::observed && !step.display) {
              return "an observed virtual display step names no display";
            }
            break;
          case journal_operation_e::remove_head:
          case journal_operation_e::restore_fields:
            return "it contains steps this version never writes";
        }
      }
      for (const auto &head : record.owned_heads) {
        if (std::none_of(record.steps.begin(), record.steps.end(), [&](const journal_step_t &step) {
              return step.operation == journal_operation_e::create_head && step.state == journal_step_state_e::observed &&
                     step.head_intent == head.identity.guid;
            })) {
          return "an owned virtual display has no observed creation";
        }
      }
      return std::nullopt;
    }

    const journal_step_t *
    unresolved_step(const journal_record_t &record) {
      const auto it = std::find_if(record.steps.begin(), record.steps.end(), [](const journal_step_t &step) {
        // A crash or failed result save leaves the same write-ahead image as an
        // accepted request whose head can still appear. Empty inventory cannot settle it.
        return step.state == journal_step_state_e::conflict ||
               (step.operation == journal_operation_e::create_head && step.state == journal_step_state_e::attempted);
      });
      return it == record.steps.end() ? nullptr : &*it;
    }

    std::uint32_t
    next_sequence(const journal_record_t &record) {
      return record.steps.empty() ? 1 : record.steps.back().sequence + 1;
    }

    native_outcome_t
    refusal(std::string reason) {
      return { native_verdict_e::refused, std::nullopt, std::move(reason) };
    }

    bool
    same_resolution(const resolution_t &a, const resolution_t &b) {
      return a.width == b.width && a.height == b.height;
    }

    /** The sole virtual head, when it carries this driver and GUID; null otherwise. */
    const observed_display_t *
    sole_head_of(const native_platform_t &platform, const inventory_t &inventory, const driver_instance_id_t &driver, const head_guid_t &guid) {
      if (!inventory.available) {
        return nullptr;
      }
      const auto heads = virtual_heads(platform, inventory);
      if (heads.size() != 1) {
        return nullptr;
      }
      const auto &head = *heads.front();
      if (!head.head || head.head->driver != driver || head.head->guid != guid || head.head->incarnation.value == 0) {
        return nullptr;
      }
      return &head;
    }

    /** Refresh rates within 1 Hz are one mode, as the legacy display path and the whole-Hz driver list treat them. */
    bool
    same_refresh(const display_device::refresh_rate_t &a, const display_device::refresh_rate_t &b) {
      if (a.denominator == 0 || b.denominator == 0) {
        return false;
      }
      const auto left = static_cast<long double>(a.numerator) * b.denominator;
      const auto right = static_cast<long double>(b.numerator) * a.denominator;
      const auto scale = static_cast<long double>(a.denominator) * b.denominator;
      return (left > right ? left - right : right - left) <= scale;
    }

    /** The head is listed with `mode`, and nothing else on the host changed. */
    bool
    settled(const observed_display_t *head, const display_mode_t &mode, bool others_unchanged) {
      return head && head->layout && same_resolution(head->layout->mode.resolution, mode.resolution) &&
             same_refresh(head->layout->mode.refresh_rate, mode.refresh_rate) && others_unchanged;
    }

    /** Why a managed process without a usable profile refuses every launch. */
    std::string
    unusable(const error_t &error) {
      if (error.code == error_code_e::resource_unavailable) {
        return error.detail;
      }
      return "Invalid display_topology_profile: " + error.detail;
    }
  }  // namespace

  std::string_view
  to_string(native_verdict_e verdict) noexcept {
    switch (verdict) {
      case native_verdict_e::ok:
        return "ok";
      case native_verdict_e::refused:
        return "refused";
      case native_verdict_e::conflict:
        return "conflict";
    }
    return "unknown";
  }

  result_t<std::optional<native_profile_t>>
  parse_native_profile(std::string_view text) {
    if (text.size() > max_profile_bytes) {
      return profile_error(error_code_e::invalid_profile, "the setting is longer than 4096 bytes");
    }
    if (text.find_first_not_of(" \t\r\n") == std::string_view::npos) {
      return std::optional<native_profile_t> {};
    }

    // nlohmann::json keeps the last of duplicate keys; P1 does not detect duplicates.
    json value;
    try {
      value = json::parse(text.begin(), text.end());
    }
    catch (const json::exception &error) {
      return profile_error(error_code_e::invalid_profile, std::string { "not valid JSON: " } + error.what());
    }
    if (!value.is_object()) {
      return profile_error(error_code_e::invalid_profile, "expected a JSON object");
    }
    if (const auto key = unknown_key(value, { "version", "monitors" })) {
      return profile_error(error_code_e::invalid_profile, "unknown key \"" + *key + "\"");
    }
    if (!value.contains("version") || !value.at("version").is_number_unsigned()) {
      return profile_error(error_code_e::invalid_profile, "\"version\" must be the number 1");
    }
    if (value.at("version").get<std::uint64_t>() != 1) {
      return profile_error(error_code_e::unsupported_version, "only version 1 is supported");
    }
    if (!value.contains("monitors") || !value.at("monitors").is_array() || value.at("monitors").empty()) {
      return profile_error(error_code_e::invalid_profile, "\"monitors\" must list exactly one monitor");
    }
    const auto &monitors = value.at("monitors");
    if (monitors.size() > 1) {
      return profile_error(error_code_e::limit_exceeded, "this version manages exactly one monitor");
    }

    const auto &monitor = monitors.front();
    if (!monitor.is_object()) {
      return profile_error(error_code_e::invalid_profile, "a monitor must be a JSON object");
    }
    if (const auto key = unknown_key(monitor, { "source", "display", "max_heads" })) {
      return profile_error(error_code_e::invalid_profile, "unknown monitor key \"" + *key + "\"");
    }
    if (!monitor.contains("source") || !monitor.at("source").is_string()) {
      return profile_error(error_code_e::invalid_profile, "a monitor needs \"source\": \"physical\" or \"virtual\"");
    }
    const auto source = monitor.at("source").get<std::string>();
    if (source == "virtual") {
      if (monitor.contains("display")) {
        return profile_error(error_code_e::invalid_profile, "a virtual monitor cannot name a display");
      }
      native_profile_t profile { native_profile_t::source_e::virtual_head, {} };
      if (monitor.contains("max_heads")) {
        const auto &value = monitor.at("max_heads");
        if (!value.is_number_unsigned() || value.get<std::uint64_t>() == 0) {
          return profile_error(error_code_e::invalid_profile, "\"max_heads\" must be a whole number of heads from 1");
        }
        if (value.get<std::uint64_t>() > max_virtual_heads) {
          return profile_error(error_code_e::limit_exceeded,
            "this version offers display groups of at most " + std::to_string(max_virtual_heads) + " heads");
        }
        profile.max_heads = static_cast<std::uint8_t>(value.get<std::uint64_t>());
      }
      return std::optional { profile };
    }
    if (monitor.contains("max_heads")) {
      return profile_error(error_code_e::invalid_profile, "only a virtual monitor offers display groups (\"max_heads\")");
    }
    if (source != "physical") {
      return profile_error(error_code_e::invalid_profile, "unknown source \"" + source + "\"");
    }
    if (!monitor.contains("display") || !monitor.at("display").is_string() ||
        monitor.at("display").get<std::string>().empty()) {
      return profile_error(error_code_e::invalid_profile, "a physical monitor needs its exact \"display\" device ID");
    }
    return std::optional { native_profile_t { native_profile_t::source_e::physical,
      os_display_id_t { monitor.at("display").get<std::string>() } } };
  }

  result_t<creation_delivery_e>
  native_platform_t::create_head(const mutation_capability_t &capability, const head_guid_t &intent) {
    capability.require(operation_e::native_apply);
    return do_create_head(capability, intent);
  }

  result_t<void>
  native_platform_t::remove_all_heads(const mutation_capability_t &capability) {
    // Preparation can roll back its own failed create without acquiring a new capability.
    capability.require(capability.kind() == command_kind_e::launch_prepare ? operation_e::native_apply : operation_e::native_restore);
    return do_remove_all_heads(capability);
  }

  result_t<void>
  native_platform_t::apply_fields(const mutation_capability_t &capability, const os_display_id_t &display, const managed_values_t &values) {
    // P1 field writes are replay only; preparing a native profile does not change host fields.
    capability.require(capability.kind() == command_kind_e::startup_recovery ? operation_e::native_recover : operation_e::native_restore);
    return do_apply_fields(capability, display, values);
  }

  result_t<void>
  native_platform_t::change_head_mode(const mutation_capability_t &capability, const observed_display_t &head, const display_mode_t &mode) {
    capability.require(operation_e::native_mode);
    return do_change_head_mode(capability, head, mode);
  }

  result_t<void>
  native_platform_t::activate_head(const mutation_capability_t &capability, const observed_display_t &head) {
    capability.require(operation_e::native_activate);
    return do_activate_head(capability, head);
  }

  operation_e
  arrange_operation(command_kind_e kind) noexcept {
    // Placing is part of preparing, restoring or recovering, never a write of its own.
    switch (kind) {
      case command_kind_e::launch_prepare:
        return operation_e::native_apply;
      case command_kind_e::startup_recovery:
        return operation_e::native_recover;
      default:
        return operation_e::native_restore;
    }
  }

  result_t<void>
  native_platform_t::arrange(const mutation_capability_t &capability, const std::vector<placement_t> &placements,
    const std::vector<os_display_id_t> &off) {
    capability.require(arrange_operation(capability.kind()));
    return do_arrange(capability, placements, off);
  }

  std::string_view
  to_string(mode_change_verdict_e verdict) noexcept {
    switch (verdict) {
      case mode_change_verdict_e::applied:
        return "applied";
      case mode_change_verdict_e::unchanged:
        return "unchanged";
      case mode_change_verdict_e::refused:
        return "refused";
      case mode_change_verdict_e::conflict:
        return "conflict";
    }
    return "unknown";
  }

  native_executor_t::native_executor_t(journal_store_t &store, native_platform_t &platform, native_executor_options_t options):
      store_(store),
      platform_(platform),
      options_(std::move(options)) {
    if (!options_.new_uuid || options_.create_attempts == 0 || !valid_uuid(options_.driver)) {
      throw std::invalid_argument { "The native executor needs a uuid source, at least one creation attempt and a driver identity" };
    }
    owner_ = owner_id_t { fresh_uuid() };
  }

  uuid_bytes_t
  native_executor_t::fresh_uuid() {
    const auto value = options_.new_uuid();
    if (value == uuid_bytes_t {}) {
      throw std::runtime_error { "The uuid source returned a null identity" };
    }
    return value;
  }

  native_health_t
  native_executor_t::recover(const mutation_capability_t &capability) {
    capability.require(operation_e::native_recover);
    try {
      const auto loaded = store_.load();
      if (!loaded.present()) {
        health_ = {};
        created_in_this_run_.reset();
      }
      else if (loaded.status != journal_load_status_e::loaded) {
        conflict("The native journal is " + std::string { to_string(loaded.status) } + ": " + loaded.detail);
      }
      else {
        // Nothing on the host is provably ours after a restart, so nothing is removed.
        reconcile(capability, *loaded.record, false);
      }
    }
    catch (const std::exception &error) {
      after_exception(std::string { "Native recovery failed: " } + error.what());
    }
    catch (...) {
      after_exception("Native recovery failed");
    }
    return health_;
  }

  native_outcome_t
  native_executor_t::prepare(const mutation_capability_t &capability, const native_profile_t &profile) {
    capability.require(operation_e::native_apply);
    try {
      if (store_.load().present()) {
        return conflict("A native journal is still present; it must be reconciled or reset first");
      }
      return profile.source == native_profile_t::source_e::virtual_head ? prepare_virtual(capability, profile.virtual_heads) :
                                                                          prepare_physical(profile.display);
    }
    catch (const std::exception &error) {
      return after_exception(std::string { "Native prepare failed: " } + error.what());
    }
    catch (...) {
      return after_exception("Native prepare failed");
    }
  }

  native_outcome_t
  native_executor_t::prepare_physical(const os_display_id_t &display) {
    const auto inventory = platform_.inventory();
    profile_t profile;
    profile.entries.push_back(monitor_entry_t { monitor_id_t {}, physical_source_t { display, std::nullopt } });
    const auto resolved = resolve_profile(profile, inventory);
    if (resolved.size() != 1 || resolved.front().state != binding_state_e::bound) {
      const auto detail = resolved.size() == 1 && resolved.front().error ? resolved.front().error->detail :
                                                                           std::string { "it could not be resolved" };
      return refused("Display " + display.value + ": " + detail);
    }
    if (platform_.is_virtual_head(*find_unique(inventory, display))) {
      return refused("Display " + display.value + " is a virtual display; use a virtual entry");
    }
    native_outcome_t outcome { native_verdict_e::ok, native_target_t { display, false }, {} };
    outcome.targets = { *outcome.target };
    return outcome;
  }

  native_outcome_t
  native_executor_t::prepare_virtual(const mutation_capability_t &capability, std::uint8_t count) {
    if (count == 0 || count > max_virtual_heads) {
      return refused("This version prepares 1 to " + std::to_string(max_virtual_heads) + " virtual displays, not " + std::to_string(count));
    }
    // ZakoVDD v0.18 shows at most its configured count of monitors; another create for an occupied slot is
    // dropped as a silent success (gh-all runs 20261108-165856/-170253), so asking would only wait and roll back.
    if (const auto capacity = platform_.head_capacity(); capacity && count > *capacity) {
      static constexpr const char *names[] = { "", "one", "two", "three", "four" };
      const auto group = count < std::size(names) ? std::string { names[count] } : std::to_string(count);
      return refused("ZakoVDD is configured for " + std::to_string(*capacity) + " display(s); " + group +
                     "-display groups need the ZakoVDD multi-monitor change");
    }
    const auto baseline = platform_.inventory();
    if (!baseline.available) {
      return refused("The display inventory is unavailable");
    }
    if (!virtual_heads(platform_, baseline).empty()) {
      return refused("A virtual display already exists; this version creates one only when none exists");
    }

    const transaction_id_t transaction { fresh_uuid() };
    const request_id_t request { fresh_uuid() };
    // Each head slot keeps its own attempts, so every request identity is fresh.
    std::vector<head_guid_t> intents;
    for (std::uint32_t attempt = 0; attempt < options_.create_attempts * count; ++attempt) {
      intents.emplace_back(fresh_uuid());
    }

    auto prepared = prepare_journal(std::nullopt, owner_, transaction, request, topology_revision_t {}, baseline);
    if (!prepared) {
      return refused("The baseline cannot be journaled: " + prepared.error().detail);
    }
    // Every attempt keeps this first baseline; nothing below re-snapshots it.
    auto record = std::move(*prepared);
    const auto expected = unmanaged_signature(platform_, record.baseline, record);
    // The request's outcome cannot be proven now, and a later readback could not prove it
    // either (its display may still appear), so the journal keeps it In conflict until reset.
    const auto unresolved = [&](std::string reason) {
      auto &step = record.steps.back();
      step.state = journal_step_state_e::conflict;
      step.detail = reason;
      if (const auto saved = store_.save(record); !saved) {
        reason += "; the native journal could not record it: " + saved.error().detail;
      }
      return conflict(std::move(reason));
    };

    // The heads this prepare owns, as last listed, in creation order. Preparation is all or
    // nothing: a failure on any head removes every head created before it.
    std::vector<observed_display_t> owned;
    const auto exactly = [this](std::vector<observed_display_t> heads) {
      return [this, heads = std::move(heads)](const inventory_t &inventory) { return exact_head_set(platform_, inventory, heads); };
    };
    const auto and_new = [&](const observed_display_t &head) {
      auto heads = owned;
      heads.push_back(head);
      return exactly(std::move(heads));
    };
    const auto give_up = [&](std::string why) {
      return owned.empty() ? clear_journal(refusal(std::move(why))) : roll_back_heads(capability, record, exactly(owned), why);
    };
    const auto ids_of = [](const std::vector<observed_display_t> &heads) {
      std::vector<os_display_id_t> ids;
      for (const auto &head : heads) ids.push_back(head.id);
      return ids;
    };
    // ZakoVDD re-enumerates the heads it already has while it adds another (gh-all run 20261107-232556):
    // the same driver head (ContainerId) on the same adapter comes back with a new OS identity and arrival.
    // That head is still this run's, so the journal follows it; anything else stays a change.
    // Set when following was proven but could not be journaled; the followed heads are still the ones to roll back.
    std::string follow_unsaved;
    const auto follow_owned = [&](const inventory_t &inventory) {
      if (!inventory.available || lists_each(platform_, inventory, owned)) return true;
      const auto listed = virtual_heads(platform_, inventory);
      std::vector<observed_display_t> followed;
      for (const auto &mine : owned) {
        const observed_display_t *match = nullptr;
        std::size_t matches = 0;
        for (const auto *head : listed) {
          if (head->head && mine.head && head->head->driver == mine.head->driver && head->head->guid == mine.head->guid &&
              head->head->incarnation.value != 0 && head->adapter && head->adapter == mine.adapter) {
            match = head;
            ++matches;
          }
        }
        if (matches != 1) return false;
        followed.push_back(*match);
      }
      std::size_t seen = 0;
      for (auto &step : record.steps) {
        if (step.operation != journal_operation_e::create_head || step.state != journal_step_state_e::observed) continue;
        if (seen >= followed.size()) return false;
        step.display = followed[seen].id;
        step.observed = inventory;
        ++seen;
      }
      if (seen != followed.size() || record.owned_heads.size() != followed.size()) return false;
      for (std::size_t index = 0; index < followed.size(); ++index) {
        record.owned_heads[index].identity = *followed[index].head;
      }
      owned = std::move(followed);
      if (const auto saved = store_.save(record); !saved) {
        follow_unsaved = "The native journal could not record a re-enumerated virtual display: " + saved.error().detail;
        return false;
      }
      return true;
    };

    for (std::uint8_t slot = 0; slot < count; ++slot) {
      const auto ordinal = std::to_string(slot + 1) + " of " + std::to_string(count);
      std::string failure;
      bool done = false;
      for (std::uint32_t attempt = 0; attempt < options_.create_attempts && !done; ++attempt) {
        const auto &intent = intents[slot * options_.create_attempts + attempt];
        record.steps.push_back({ next_sequence(record), journal_operation_e::create_head, std::nullopt, intent,
          journal_step_state_e::attempted, std::nullopt, {} });
        // Write-ahead: the attempt is durable before the driver is asked.
        if (const auto saved = store_.save(record); !saved) {
          return give_up("The native journal could not be saved: " + saved.error().detail);
        }

        const auto created = platform_.create_head(capability, intent);
        const auto after = platform_.inventory();
        if (!after.available) {
          return unresolved("The display inventory is unavailable after asking for a virtual display");
        }
        const auto heads = virtual_heads(platform_, after);
        if (heads.size() > owned.size() + 1) {
          return conflict("More than one virtual display appeared, so none can be proven to be this host's");
        }
        const auto this_runs_heads = [&](const inventory_t &inventory) {
          return only_requested_heads(platform_, inventory, options_.driver, intents);
        };
        if (!follow_owned(after)) {
          // An earlier head changed beyond a re-enumeration, so it is no longer proven and everything
          // this run asked for is removed: the driver clears its whole list, including a head still on its way.
          auto why = follow_unsaved.empty() ? "Creating virtual display " + ordinal + " changed a virtual display this host created" : follow_unsaved;
          const bool request_listed = std::any_of(heads.begin(), heads.end(), [&](const observed_display_t *head) {
            return head->head && head->head->guid == intent;
          });
          if (created && !request_listed) {
            why += ", and its own display did not appear";
          }
          return roll_back_heads(capability, record, this_runs_heads, why);
        }
        const auto fresh = std::find_if(heads.begin(), heads.end(), [&](const observed_display_t *head) {
          return std::none_of(owned.begin(), owned.end(), [&](const observed_display_t &mine) { return mine.id == head->id; });
        });
        if (fresh == heads.end()) {
          if (created && *created == creation_delivery_e::accepted) {
            // Not listed within the platform's creation wait, though the driver has it. It may still be
            // listed later, so the driver's whole list is cleared now rather than waiting on a late arrival.
            const auto explained = platform_.explain_missing_head(intent);
            return roll_back_heads(capability, record, this_runs_heads,
              "The driver accepted the request but no virtual display appeared" + (explained.empty() ? std::string {} : ": " + explained));
          }
          if (created) {
            return unresolved("The driver delivery is uncertain and no virtual display appeared");
          }
          // The driver refused the request, so no display of it can appear later.
          failure = created.error().detail.empty() ? std::string { "the driver reported an error" } : created.error().detail;
          auto &step = record.steps.back();
          step.state = journal_step_state_e::failed;
          step.detail = failure;
          if (unmanaged_signature(platform_, after, record) != expected) {
            return give_up("Other displays changed while creating a virtual display: " + failure);
          }
          continue;
        }

        const auto &head = **fresh;
        if (!created) {
          return unresolved("The driver refused the request but a virtual display appeared: " + created.error().detail);
        }
        // Only a head carrying this request's full identity can be ours; anything else is left alone.
        if (!head.head || head.head->driver != options_.driver || head.head->guid != intent || head.head->incarnation.value == 0) {
          return unresolved("A virtual display appeared that does not carry this request's identity");
        }
        if (*created != creation_delivery_e::accepted) {
          return roll_back_heads(capability, record, and_new(head), "The driver delivery is uncertain");
        }
        // Only corners and the primary may move here, and displays that were off at baseline may have been
        // turned on (Windows' saved layout for the new display set); all of it is put back once the head is
        // journaled.
        if (!turned_on(platform_, after, record)) {
          return roll_back_heads(capability, record, and_new(head), with_changes("Creating the virtual display changed other displays", platform_, after, record));
        }
        auto &step = record.steps.back();
        step.state = journal_step_state_e::observed;
        step.display = head.id;
        step.observed = after;
        record.owned_heads.push_back({ *head.head, owner_, head.adapter.value_or(adapter_luid_t {}), 0 });
        if (const auto saved = store_.save(record); !saved) {
          return roll_back_heads(capability, record, and_new(head), "The native journal could not record the new virtual display: " + saved.error().detail);
        }
        auto placed = after;
        auto what = std::string { "Creating the virtual display changed other displays" };
        // A cold-created head can be listed but not on the desktop, with nothing to capture.
        // It is already owned in the journal, so any failure below removes it again.
        if (!on_desktop(head)) {
          const auto activated = platform_.activate_head(capability, head);
          if (!activated) {
            return roll_back_heads(capability, record, and_new(head), "The new virtual display could not be added to the desktop: " + activated.error().detail);
          }
          const auto active = platform_.inventory();
          auto with_head = owned;
          with_head.push_back(head);
          if (!exact_head_set(platform_, active, with_head)) {
            return roll_back_heads(capability, record, and_new(head), "The virtual display changed identity while being added to the desktop");
          }
          const auto *now = find_unique(active, head.id);
          if (!on_desktop(*now)) {
            return roll_back_heads(capability, record, and_new(head), "The virtual display was not added to the desktop");
          }
          if (!turned_on(platform_, active, record)) {
            return roll_back_heads(capability, record, and_new(head), with_changes("Adding the virtual display to the desktop changed other displays", platform_, active, record));
          }
          record.steps.back().observed = active;
          if (const auto saved = store_.save(record); !saved) {
            return roll_back_heads(capability, record, and_new(head), "The native journal could not record the activated virtual display: " + saved.error().detail);
          }
          placed = active;
          what = "Adding the virtual display to the desktop changed other displays";
        }
        auto heads_now = ids_of(owned);
        heads_now.push_back(head.id);
        if (const auto error = place(platform_, capability, record, heads_now, placed, and_new(head), what, true)) {
          return roll_back_heads(capability, record, and_new(head), *error);
        }
        owned.push_back(*find_unique(placed, head.id));
        done = true;
      }
      if (!done) {
        return give_up((count == 1 ? std::string { "No virtual display was created: " } :
                                     "Virtual display " + ordinal + " was not created: ") + failure);
      }
    }

    std::optional<inventory_t> settled_placement;
    if (count > 1) {
      // Adding a head can move or deactivate the ones added before it.
      const auto settled_heads = platform_.inventory();
      if (!follow_owned(settled_heads) || !exact_head_set(platform_, settled_heads, owned)) {
        return roll_back_heads(capability, record, exactly(owned), follow_unsaved.empty() ? "The virtual displays changed while they were being prepared" : follow_unsaved);
      }
      for (std::size_t index = 0; index < owned.size(); ++index) {
        if (!on_desktop(*find_unique(settled_heads, owned[index].id))) {
          return roll_back_heads(capability, record, exactly(owned),
            "Virtual display " + std::to_string(index + 1) + " left the desktop while the others were added");
        }
      }
      auto placed = settled_heads;
      if (const auto error = place(platform_, capability, record, ids_of(owned), placed, exactly(owned), "Adding the virtual displays changed other displays", true)) {
        return roll_back_heads(capability, record, exactly(owned), *error);
      }
      settled_placement = std::move(placed);
    }

    created_in_this_run_ = record.transaction;
    health_ = { true, false, {} };
    native_outcome_t outcome { native_verdict_e::ok, std::nullopt, {} };
    for (const auto &head : owned) {
      // Where each head ended up: the last verified placement of the whole set, or its own.
      const auto *now = settled_placement ? find_unique(*settled_placement, head.id) : &head;
      outcome.targets.push_back({ head.id, true, now ? now->layout : std::nullopt });
    }
    outcome.target = outcome.targets.front();
    return outcome;
  }

  native_outcome_t
  native_executor_t::roll_back_heads(const mutation_capability_t &capability, const journal_record_t &record,
    const std::function<bool(const inventory_t &)> &proven, const std::string &why) {
    // Same-run proof: no virtual display existed before, the heads listed now are provably
    // this run's, and the coordinator serializes every other virtual display operation.
    if (!proven(platform_.inventory())) {
      return conflict(why + "; the virtual display could not be proven to be this host's before rollback");
    }
    if (const auto removed = platform_.remove_all_heads(capability); !removed) {
      return conflict(why + "; removing it failed: " + removed.error().detail);
    }
    auto after = platform_.inventory();
    if (!after.available || !virtual_heads(platform_, after).empty()) {
      return conflict(why + "; its removal could not be confirmed");
    }
    // The driver's list is empty now, so no request of this run can still appear: its creations are settled. If the
    // layout cannot be put back below, a later restore retries just that (gh-all run 20261109-174516).
    auto settled = record;
    bool resolved = false;
    for (auto &step : settled.steps) {
      if (step.operation == journal_operation_e::create_head && step.state == journal_step_state_e::attempted) {
        step.state = journal_step_state_e::failed;
        step.detail = "removed by the rollback: " + why;
        resolved = true;
      }
    }
    if (const auto error = put_back(capability, settled, after, why + "; other displays still differ from the baseline after removing it", false)) {
      std::string unsaved;
      if (resolved) {
        if (const auto saved = store_.save(settled); !saved) {
          unsaved = "; the native journal could not record the removal: " + saved.error().detail;
        }
      }
      return conflict(*error + unsaved);
    }
    return clear_journal(refusal(why));
  }

  native_outcome_t
  native_executor_t::restore(const mutation_capability_t &capability) {
    capability.require(operation_e::native_restore);
    try {
      const auto loaded = store_.load();
      if (!loaded.present()) {
        health_ = {};
        created_in_this_run_.reset();
        return {};
      }
      if (loaded.status != journal_load_status_e::loaded) {
        return conflict("The native journal is " + std::string { to_string(loaded.status) } + ": " + loaded.detail);
      }
      const auto &record = *loaded.record;
      return reconcile(capability, record, record.owner == owner_ && created_in_this_run_ == record.transaction);
    }
    catch (const std::exception &error) {
      return after_exception(std::string { "Native restore failed: " } + error.what());
    }
    catch (...) {
      return after_exception("Native restore failed");
    }
  }

  native_health_t
  native_executor_t::reset(const mutation_capability_t &capability) {
    capability.require(operation_e::reset_persistence);
    try {
      if (const auto cleared = store_.clear(); !cleared) {
        health_ = { true, true, "The native journal could not be removed: " + cleared.error().detail };
        return health_;
      }
    }
    catch (const std::exception &error) {
      health_ = { true, true, std::string { "The native journal could not be removed: " } + error.what() };
      return health_;
    }
    catch (...) {
      health_ = { true, true, "The native journal could not be removed" };
      return health_;
    }
    health_ = {};
    created_in_this_run_.reset();
    return health_;
  }

  mode_change_outcome_t
  native_executor_t::change_mode(const mutation_capability_t &capability, const resolution_t &resolution) {
    capability.require(operation_e::native_mode);
    const auto refuse = [](std::string reason) {
      return mode_change_outcome_t { mode_change_verdict_e::refused, std::nullopt, std::move(reason) };
    };
    const auto in_conflict = [this](std::string reason) {
      conflict(reason);
      return mode_change_outcome_t { mode_change_verdict_e::conflict, std::nullopt, std::move(reason) };
    };
    try {
      if (health_.in_conflict) {
        return refuse("The native topology is In conflict: " + health_.reason);
      }
      if (resolution.width == 0 || resolution.height == 0) {
        return refuse("The requested resolution is empty");
      }
      const auto loaded = store_.load();
      if (!loaded.present() || loaded.status != journal_load_status_e::loaded) {
        return refuse("No virtual display of this run is present");
      }
      const auto &record = *loaded.record;
      if (record.owner != owner_ || created_in_this_run_ != record.transaction) {
        return refuse("The virtual display was not created by this run");
      }
      if (semantic_problem(record) || unresolved_step(record)) {
        return refuse("The native journal does not prove a single virtual display");
      }
      if (record.owned_heads.size() > 1) {
        // Per-head mode changes need per-track client control (P5); no head is touched.
        return refuse("Changing the mode of one of several virtual displays is not supported yet");
      }
      const auto *created = observed_create(record);
      const auto *expected = created && created->display && created->observed ? find_unique(*created->observed, *created->display) : nullptr;
      if (record.owned_heads.size() != 1 || !expected || !expected->head) {
        return refuse("The native journal does not prove a single virtual display");
      }
      const auto current = platform_.inventory();
      if (!sole_matching_head(platform_, current, *expected)) {
        return refuse("The virtual display no longer matches the one this run created");
      }
      const auto *head = find_unique(current, expected->id);
      if (!head->layout) {
        return refuse("The virtual display is not active");
      }
      const auto previous = head->layout->mode;
      if (same_resolution(previous.resolution, resolution)) {
        return { mode_change_verdict_e::unchanged, head->id, {}, head->layout };
      }

      const auto guid = expected->head->guid;
      const auto others = unmanaged_signature(platform_, current, record);
      const auto unchanged_others = [&](const inventory_t &inventory) {
        return unmanaged_signature(platform_, inventory, record) == others;
      };
      auto target = previous;
      target.resolution = resolution;
      const auto changed = platform_.change_head_mode(capability, *head, target);
      const auto after = platform_.inventory();
      const auto *now = sole_head_of(platform_, after, options_.driver, guid);
      if (changed && settled(now, target, unchanged_others(after))) {
        return re_anchor(record, *now, after, mode_change_verdict_e::applied, {});
      }

      auto why = changed ? std::string { "The virtual display did not take the requested mode" } :
                           "Changing the virtual display mode failed: " + changed.error().detail;
      // Only a change that was definitely never sent can be trusted not to land later.
      const bool never_sent = !changed && changed.error().code != error_code_e::unknown_applied_state;
      if (never_sent && settled(now, previous, unchanged_others(after))) {
        return re_anchor(record, *now, after, mode_change_verdict_e::refused, std::move(why));
      }
      if (!now) {
        return in_conflict(why + "; the virtual display can no longer be proven to be this host's");
      }
      const auto rolled = platform_.change_head_mode(capability, *now, previous);
      const auto restored = platform_.inventory();
      const auto *back = sole_head_of(platform_, restored, options_.driver, guid);
      if (rolled && settled(back, previous, unchanged_others(restored))) {
        return re_anchor(record, *back, restored, mode_change_verdict_e::refused, why + "; the previous mode was restored");
      }
      return in_conflict(why + "; the previous mode could not be restored" + (rolled ? std::string {} : ": " + rolled.error().detail));
    }
    catch (const std::exception &error) {
      return in_conflict(std::string { "Changing the virtual display mode failed: " } + error.what());
    }
    catch (...) {
      return in_conflict("Changing the virtual display mode failed");
    }
  }

  mode_change_outcome_t
  native_executor_t::change_mode(const mutation_capability_t &capability, std::size_t index, const resolution_t &resolution) {
    capability.require(operation_e::native_mode);
    const auto refuse = [](std::string reason) {
      return mode_change_outcome_t { mode_change_verdict_e::refused, std::nullopt, std::move(reason) };
    };
    const auto in_conflict = [this](std::string reason) {
      conflict(reason);
      return mode_change_outcome_t { mode_change_verdict_e::conflict, std::nullopt, std::move(reason) };
    };
    try {
      if (health_.in_conflict) {
        return refuse("The native topology is In conflict: " + health_.reason);
      }
      if (resolution.width == 0 || resolution.height == 0) {
        return refuse("The requested resolution is empty");
      }
      const auto loaded = store_.load();
      if (!loaded.present() || loaded.status != journal_load_status_e::loaded) {
        return refuse("No virtual display of this run is present");
      }
      const auto &record = *loaded.record;
      if (record.owner != owner_ || created_in_this_run_ != record.transaction) {
        return refuse("The virtual display was not created by this run");
      }
      if (semantic_problem(record) || unresolved_step(record)) {
        return refuse("The native journal does not prove its virtual displays");
      }
      if (index >= std::max<std::size_t>(record.owned_heads.size(), 1)) {
        return refuse("The requested virtual display is not one this run created");
      }
      if (record.owned_heads.size() <= 1) {
        return change_mode(capability, resolution);
      }
      const auto owned = owned_as_observed(record);
      if (!owned) {
        return refuse("The native journal does not prove its virtual displays");
      }
      const auto current = platform_.inventory();
      if (!exact_head_set(platform_, current, *owned)) {
        return refuse("The virtual displays no longer match the ones this run created");
      }
      const auto *head = find_unique(current, (*owned)[index].id);
      if (!head || !head->layout) {
        return refuse("The virtual display is not active");
      }
      const auto previous = head->layout->mode;
      if (same_resolution(previous.resolution, resolution)) {
        return { mode_change_verdict_e::unchanged, head->id, {}, head->layout };
      }

      // The other heads of the set, exactly as listed now: same identity, state and mode afterwards.
      std::vector<observed_display_t> siblings;
      for (std::size_t i = 0; i < owned->size(); ++i) {
        if (i == index) {
          continue;
        }
        const auto *sibling = find_unique(current, (*owned)[i].id);
        if (!sibling) {
          return refuse("The virtual displays no longer match the ones this run created");
        }
        siblings.push_back(*sibling);
      }
      const auto siblings_unchanged = [&](const inventory_t &inventory) {
        if (!lists_each(platform_, inventory, siblings) || virtual_heads(platform_, inventory).size() != owned->size()) {
          return false;
        }
        return std::all_of(siblings.begin(), siblings.end(), [&](const observed_display_t &was) {
          const auto *now = find_unique(inventory, was.id);
          return now && now->info.device_state == was.info.device_state && now->layout.has_value() == was.layout.has_value() &&
                 (!was.layout || (same_resolution(now->layout->mode.resolution, was.layout->mode.resolution) &&
                                   same_refresh(now->layout->mode.refresh_rate, was.layout->mode.refresh_rate)));
        });
      };
      const auto guid = head->head->guid;
      const auto adapter = record.owned_heads[index].adapter;
      const auto others = unmanaged_signature(platform_, current, record);
      const auto unchanged_others = [&](const inventory_t &inventory) {
        return unmanaged_signature(platform_, inventory, record) == others && siblings_unchanged(inventory);
      };
      // This run's head: its GUID once, on the journal's adapter, under an OS id nothing else carries.
      // Anything less is not proven ours, so it is neither rolled back nor re-anchored.
      const auto owned_head = [&](const inventory_t &inventory) -> const observed_display_t * {
        if (!inventory.available) {
          return nullptr;
        }
        const observed_display_t *found = nullptr;
        for (const auto *listed : virtual_heads(platform_, inventory)) {
          if (!listed->head || listed->head->driver != options_.driver || listed->head->guid != guid || listed->head->incarnation.value == 0) {
            continue;
          }
          if (found) {
            return nullptr;
          }
          found = listed;
        }
        if (!found || !found->adapter || *found->adapter != adapter || find_unique(inventory, found->id) != found) {
          return nullptr;
        }
        return found;
      };

      auto target = previous;
      target.resolution = resolution;
      const auto changed = platform_.change_head_mode(capability, *head, target);
      const auto after = platform_.inventory();
      const auto *now = owned_head(after);
      if (changed && settled(now, target, unchanged_others(after))) {
        return re_anchor_head(record, index, *now, after, mode_change_verdict_e::applied, {});
      }

      auto why = changed ? std::string { "The virtual display did not take the requested mode" } :
                           "Changing the virtual display mode failed: " + changed.error().detail;
      const bool never_sent = !changed && changed.error().code != error_code_e::unknown_applied_state;
      if (never_sent && settled(now, previous, unchanged_others(after))) {
        return re_anchor_head(record, index, *now, after, mode_change_verdict_e::refused, std::move(why));
      }
      if (!now) {
        return in_conflict(why + "; the virtual display can no longer be proven to be this host's");
      }
      const auto rolled = platform_.change_head_mode(capability, *now, previous);
      const auto restored = platform_.inventory();
      const auto *back = owned_head(restored);
      if (rolled && settled(back, previous, unchanged_others(restored))) {
        return re_anchor_head(record, index, *back, restored, mode_change_verdict_e::refused, why + "; the previous mode was restored");
      }
      return in_conflict(why + "; the previous mode could not be restored" + (rolled ? std::string {} : ": " + rolled.error().detail));
    }
    catch (const std::exception &error) {
      return in_conflict(std::string { "Changing the virtual display mode failed: " } + error.what());
    }
    catch (...) {
      return in_conflict("Changing the virtual display mode failed");
    }
  }

  mode_change_outcome_t
  native_executor_t::re_anchor_head(journal_record_t record, std::size_t index, const observed_display_t &head, const inventory_t &after,
    mode_change_verdict_e verdict, std::string reason) {
    const auto fail = [&](const std::string &detail) {
      const auto full = (reason.empty() ? std::string {} : reason + "; ") + detail;
      conflict(full);
      return mode_change_outcome_t { mode_change_verdict_e::conflict, std::nullopt, full };
    };
    journal_step_t *created = nullptr;
    std::size_t seen = 0;
    for (auto &step : record.steps) {
      if (step.operation == journal_operation_e::create_head && step.state == journal_step_state_e::observed && seen++ == index) {
        created = &step;
        break;
      }
    }
    if (!created || index >= record.owned_heads.size() || !head.head) {
      return fail("the native journal no longer names the virtual display it created");
    }
    auto &owned = record.owned_heads[index];
    // The journal keeps its slot; it must not claim a head on another adapter, or one whose adapter is unknown.
    if (!head.adapter || *head.adapter != owned.adapter) {
      return fail("the virtual display is no longer on the adapter the native journal names");
    }
    created->display = head.id;
    created->observed = after;
    owned.identity = *head.head;
    if (const auto saved = store_.save(record); !saved) {
      return fail("the native journal could not record the virtual display as it is now: " + saved.error().detail);
    }
    return { verdict, head.id, std::move(reason), head.layout };
  }

  mode_change_outcome_t
  native_executor_t::re_anchor(journal_record_t record, const observed_display_t &head, const inventory_t &after,
    mode_change_verdict_e verdict, std::string reason) {
    auto created = std::find_if(record.steps.rbegin(), record.steps.rend(), [](const journal_step_t &step) {
      return step.operation == journal_operation_e::create_head && step.state == journal_step_state_e::observed;
    });
    if (created == record.steps.rend() || record.owned_heads.empty()) {
      const auto detail = "the native journal no longer names the virtual display it created";
      conflict(detail);
      return { mode_change_verdict_e::conflict, std::nullopt, detail };
    }
    auto &owned = record.owned_heads.front();
    // The journal keeps its slot; it must not claim a head on another adapter, or one whose adapter is unknown.
    if (!head.adapter || *head.adapter != owned.adapter) {
      const auto detail = (reason.empty() ? std::string {} : reason + "; ") +
                          "the virtual display is no longer on the adapter the native journal names";
      conflict(detail);
      return { mode_change_verdict_e::conflict, std::nullopt, detail };
    }
    created->display = head.id;
    created->observed = after;
    owned.identity = *head.head;
    if (const auto saved = store_.save(record); !saved) {
      const auto detail = (reason.empty() ? std::string {} : reason + "; ") +
                          "the native journal could not record the virtual display as it is now: " + saved.error().detail;
      conflict(detail);
      return { mode_change_verdict_e::conflict, std::nullopt, detail };
    }
    return { verdict, head.id, std::move(reason), head.layout };
  }

  native_outcome_t
  native_executor_t::reconcile(const mutation_capability_t &capability, const journal_record_t &record, bool same_run) {
    if (const auto problem = semantic_problem(record)) {
      return conflict("The native journal cannot be reconciled: " + *problem);
    }
    if (const auto *step = unresolved_step(record)) {
      const auto detail = step->detail.empty() ? "unconfirmed virtual display creation" : step->detail;
      return conflict("An earlier native operation was left unresolved (" + detail + "); check the host, then reset");
    }
    const auto current = platform_.inventory();
    if (!current.available) {
      return conflict("The display inventory is unavailable, so the native journal is kept");
    }

    // Decide every write before making any, so a conflict anywhere leaves the host untouched.
    std::vector<std::pair<os_display_id_t, managed_values_t>> restores;
    for (const auto &managed : record.managed) {
      const auto &id = managed.source.display;
      if (!attempted(record, journal_operation_e::apply_fields, &id)) {
        // Write-ahead: a display whose write was never attempted was never touched.
        continue;
      }
      const auto *display = find_unique(current, id);
      if (!display || !display->layout || display->adapter != managed.source.adapter ||
          display->incarnation != managed.source.incarnation ||
          display->head.has_value() != managed.source.head.has_value() ||
          (display->head && *display->head != managed.source.head->identity)) {
        return conflict("Managed display " + id.value + " is missing or is no longer the display this host changed");
      }
      const auto now = current_values(*display);
      managed_values_t restore;
      for (const auto field : all_fields) {
        if (!has(managed.intended, field) || same(now, managed.baseline, field)) {
          continue;
        }
        // The value this host left: what it read back when it did, otherwise what it asked for.
        const bool confirmed = managed.confirmed && has(*managed.confirmed, field);
        if (!same(now, confirmed ? *managed.confirmed : managed.intended, field)) {
          return conflict("The " + field_name(field) + " of display " + id.value + " was changed by something other than this host");
        }
        copy_field(restore, managed.baseline, field);
      }
      if (restore != managed_values_t {}) {
        restores.emplace_back(id, restore);
      }
    }

    std::vector<observed_display_t> owned_displays;
    if (attempted(record, journal_operation_e::create_head, nullptr)) {
      const auto heads = virtual_heads(platform_, current);
      if (!heads.empty()) {
        if (!same_run) {
          return conflict("A virtual display from an earlier run cannot be proven to be this host's, so it is left in place");
        }
        const auto expected = owned_as_observed(record);
        if (!expected || !exact_head_set(platform_, current, *expected)) {
          return conflict("The virtual displays present no longer match the one this host created, so none is removed");
        }
        owned_displays = *expected;
      }
    }

    for (const auto &[id, values] : restores) {
      if (const auto applied = platform_.apply_fields(capability, id, values); !applied) {
        return conflict("Restoring display " + id.value + " failed: " + applied.error().detail);
      }
    }
    if (!owned_displays.empty()) {
      if (!restores.empty()) {
        // The restores changed the topology, so the proof is repeated before removing.
        const auto before = platform_.inventory();
        if (!exact_head_set(platform_, before, owned_displays)) {
          return conflict("The virtual display could not be proven to be this host's after restoring fields, so it is left in place");
        }
      }
      if (const auto removed = platform_.remove_all_heads(capability); !removed) {
        return conflict("Removing the virtual display failed: " + removed.error().detail);
      }
    }
    if (restores.empty() && owned_displays.empty()) {
      // Nothing was written, so the first observation is the readback.
      auto now = current;
      if (const auto error = put_back(capability, record, now, "Other displays differ from the baseline, so the native journal is kept", true)) {
        return conflict(*error);
      }
      return clear_journal({});
    }

    auto after = platform_.inventory();
    if (!after.available) {
      return conflict("The display inventory is unavailable after restoring, so the native journal is kept");
    }
    for (const auto &[id, values] : restores) {
      const auto *display = find_unique(after, id);
      if (!display || !display->layout) {
        return conflict("Display " + id.value + " is missing after it was restored");
      }
      const auto now = current_values(*display);
      for (const auto field : all_fields) {
        if (has(values, field) && !same(now, values, field)) {
          return conflict("The " + field_name(field) + " of display " + id.value + " did not return to its baseline");
        }
      }
    }
    if (!owned_displays.empty() && !virtual_heads(platform_, after).empty()) {
      return conflict("The virtual display is still present after its removal");
    }
    if (const auto error = put_back(capability, record, after, "Other displays differ from the baseline after restoring", true)) {
      return conflict(*error);
    }
    return clear_journal({});
  }

  std::optional<std::string>
  native_executor_t::put_back(const mutation_capability_t &capability, const journal_record_t &record, inventory_t &after,
    const std::string &what, bool keep_moves) {
    if (unmanaged_signature(platform_, after, record) == unmanaged_signature(platform_, record.baseline, record) &&
        (keep_moves || !movable(platform_, after, record))) {
      // The same displays in the same states. Within one prepare only this host moved them; at
      // restore or recovery the user may have, so only a changed primary or state is put back.
      return std::nullopt;
    }
    const auto no_heads = [this](const inventory_t &inventory) { return inventory.available && virtual_heads(platform_, inventory).empty(); };
    if (const auto off = turned_on(platform_, after, record); off && !off->empty() && no_heads(after)) {
      // Windows applied its saved layout for the remaining displays when the last virtual head went,
      // turning on displays that were off at baseline (and moving the primary). Put the baseline
      // desktop back, active set and corners together, in one write.
      const auto plan = placement_plan(platform_, record, after, {});
      if (!plan) {
        return what + "; the baseline desktop could not be planned";
      }
      if (const auto arranged = platform_.arrange(capability, *plan, *off); !arranged) {
        return what + "; turning the displays that were off back off failed: " + arranged.error().detail;
      }
      after = platform_.inventory();
      if (!no_heads(after)) {
        return what + "; a virtual display appeared while the baseline desktop was put back";
      }
      if (misplaced(platform_, after, record, *plan)) {
        return what + "; putting the baseline desktop back did not restore it";
      }
      return std::nullopt;
    }
    return place(platform_, capability, record, {}, after, no_heads, what, false);
  }

  native_outcome_t
  native_executor_t::conflict(std::string reason) {
    health_ = { true, true, reason };
    return { native_verdict_e::conflict, std::nullopt, std::move(reason) };
  }

  native_outcome_t
  native_executor_t::refused(std::string reason) {
    health_ = {};
    created_in_this_run_.reset();
    return refusal(std::move(reason));
  }

  native_outcome_t
  native_executor_t::clear_journal(native_outcome_t success) {
    if (const auto cleared = store_.clear(); !cleared) {
      return conflict((success.reason.empty() ? std::string {} : success.reason + "; ") +
                      "the native journal could not be removed: " + cleared.error().detail);
    }
    health_ = {};
    created_in_this_run_.reset();
    return success;
  }

  native_outcome_t
  native_executor_t::after_exception(const std::string &what) {
    bool present = true;
    try {
      present = store_.load().present();
    }
    catch (...) {
    }
    return present ? conflict(what) : refused(what);
  }

  managed_native_t::managed_native_t(native_executor_t *executor, result_t<native_profile_t> profile):
      executor_(executor),
      profile_(std::move(profile)) {
    if (profile_ && !executor_) {
      throw std::invalid_argument { "a valid display_topology_profile needs a native executor" };
    }
  }

  native_health_t
  managed_native_t::health() const {
    auto health = executor_ ? executor_->health() : native_health_t {};
    if (!profile_ && !health.in_conflict) {
      health.in_conflict = true;
      health.reason = unusable(profile_.error());
    }
    return health;
  }

  native_health_t
  managed_native_t::recover(const mutation_capability_t &capability) {
    capability.require(operation_e::native_recover);
    if (executor_) {
      executor_->recover(capability);
    }
    return health();
  }

  native_health_t
  managed_native_t::restore(const mutation_capability_t &capability) {
    capability.require(operation_e::native_restore);
    if (executor_) {
      executor_->restore(capability);
    }
    return health();
  }

  native_health_t
  managed_native_t::reset(const mutation_capability_t &capability) {
    capability.require(operation_e::reset_persistence);
    if (executor_) {
      executor_->reset(capability);
    }
    return health();
  }

  native_outcome_t
  managed_native_t::prepare(const mutation_capability_t &capability) {
    capability.require(operation_e::native_apply);
    if (!profile_) {
      return { native_verdict_e::conflict, std::nullopt, unusable(profile_.error()) };
    }
    return executor_->prepare(capability, *profile_);
  }

  native_outcome_t
  managed_native_t::prepare(const mutation_capability_t &capability, std::uint8_t heads) {
    capability.require(operation_e::native_apply);
    if (!profile_) {
      return { native_verdict_e::conflict, std::nullopt, unusable(profile_.error()) };
    }
    if (heads == 0 || heads > group_max_heads()) {
      return { native_verdict_e::refused, std::nullopt,
        "The display profile offers display groups of at most " + std::to_string(group_max_heads()) + " heads, not " +
          std::to_string(heads) };
    }
    auto profile = *profile_;
    profile.virtual_heads = heads;
    return executor_->prepare(capability, profile);
  }

  std::uint8_t
  managed_native_t::group_max_heads() const noexcept {
    return profile_ && profile_->source == native_profile_t::source_e::virtual_head ? profile_->max_heads : 0;
  }

  mode_change_outcome_t
  managed_native_t::change_mode(const mutation_capability_t &capability, const resolution_t &resolution) {
    capability.require(operation_e::native_mode);
    if (!profile_) {
      return { mode_change_verdict_e::refused, std::nullopt, unusable(profile_.error()) };
    }
    return executor_->change_mode(capability, resolution);
  }

  mode_change_outcome_t
  managed_native_t::change_mode(const mutation_capability_t &capability, std::size_t head, const resolution_t &resolution) {
    capability.require(operation_e::native_mode);
    if (!profile_) {
      return { mode_change_verdict_e::refused, std::nullopt, unusable(profile_.error()) };
    }
    return executor_->change_mode(capability, head, resolution);
  }

  head_geometry_t
  to_head_geometry(const layout_t &layout) {
    const auto &rate = layout.mode.refresh_rate;
    const std::uint64_t millihz = rate.denominator == 0 ? 0 :
                                                          (std::uint64_t { rate.numerator } * 1000 + rate.denominator / 2) / rate.denominator;
    return {
      layout.position.x,
      layout.position.y,
      layout.mode.resolution.width,
      layout.mode.resolution.height,
      static_cast<std::uint32_t>(std::min<std::uint64_t>(millihz, UINT32_MAX)),
    };
  }

  std::shared_ptr<managed_native_t>
  bind_native_profile(std::string_view setting, const native_executor_factory_t &make_executor) {
    auto parsed = parse_native_profile(setting);
    if (parsed && !*parsed) {
      return nullptr;
    }
    std::shared_ptr<native_executor_t> executor;
    std::string unavailable;
    try {
      executor = make_executor ? make_executor() : nullptr;
      if (!executor) {
        unavailable = "Native display topology is not available on this host";
      }
    }
    catch (const std::exception &error) {
      unavailable = std::string { "Native display topology is not available: " } + error.what();
    }
    catch (...) {
      unavailable = "Native display topology is not available";
    }
    result_t<native_profile_t> profile = parsed ? result_t<native_profile_t> { **parsed } : std::unexpected(parsed.error());
    if (profile && !executor) {
      profile = std::unexpected(error_t { error_code_e::resource_unavailable, unavailable, std::nullopt });
    }
    struct bound_t {
      std::shared_ptr<native_executor_t> executor;
      managed_native_t managed;
    };
    auto bound = std::make_shared<bound_t>(bound_t { executor, managed_native_t { executor.get(), std::move(profile) } });
    return { bound, &bound->managed };
  }

  topology_status_report_t
  describe_topology_status(std::string_view desired_setting, std::string_view applied_setting, const coordinator_status_t &status) {
    topology_status_report_t report;
    const auto desired = parse_native_profile(desired_setting);
    const auto applied = parse_native_profile(applied_setting);
    if (!desired) {
      report.desired = "invalid";
      report.desired_error = desired.error().detail;
    }
    else {
      report.desired = desired->has_value() ? "managed" : "legacy";
    }
    if (!status.managed_mode) {
      report.applied = "legacy";
    }
    else {
      report.applied = status.native.in_conflict ? "in_conflict" : "managed";
    }
    // Compare meanings when both parse, so whitespace or key order alone needs no restart.
    report.restart_required = desired && applied ? *desired != *applied : desired_setting != applied_setting;
    report.journal_present = status.native.journal_present;
    report.reason = status.native.reason;
    report.attachments = status.lease ? status.lease->attachments : 0;
    return report;
  }

}  // namespace display_device::topology
