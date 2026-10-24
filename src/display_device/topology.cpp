#include "topology.h"

#include <algorithm>
#include <limits>
#include <set>

namespace display_device::topology {
  namespace {
    bool
    same_resolution(const resolution_t &a, const resolution_t &b) {
      return a.width == b.width && a.height == b.height;
    }

    bool
    same_rate(const refresh_rate_t &a, const refresh_rate_t &b) {
      return a.numerator == b.numerator && a.denominator == b.denominator;
    }

    bool
    same_mode(const display_mode_t &a, const display_mode_t &b) {
      return same_resolution(a.resolution, b.resolution) && same_rate(a.refresh_rate, b.refresh_rate);
    }

    template <class T, class Equal>
    bool
    same_optional(const boost::optional<T> &a, const boost::optional<T> &b, Equal equal) {
      return a.has_value() == b.has_value() && (!a || equal(*a, *b));
    }

    std::unexpected<error_t>
    fail(error_code_e code, std::string detail, std::optional<monitor_id_t> monitor = {}) {
      return std::unexpected(error_t { code, std::move(detail), monitor });
    }

    bool
    overlaps(const layout_t &a, const layout_t &b) {
      const auto right = [](const layout_t &l) { return std::int64_t(l.position.x) + l.mode.resolution.width; };
      const auto bottom = [](const layout_t &l) { return std::int64_t(l.position.y) + l.mode.resolution.height; };
      return a.position.x < right(b) && b.position.x < right(a) &&
             a.position.y < bottom(b) && b.position.y < bottom(a);
    }

    result_t<std::uint64_t>
    validate_layout(const layout_t &layout, const limits_t &limits, monitor_id_t id) {
      const auto &size = layout.mode.resolution;
      const auto &rate = layout.mode.refresh_rate;
      if (size.width == 0 || size.height == 0) {
        return fail(error_code_e::invalid_geometry, "Display dimensions must be nonzero", id);
      }
      const auto right = std::int64_t(layout.position.x) + size.width;
      const auto bottom = std::int64_t(layout.position.y) + size.height;
      if (right > std::numeric_limits<std::int32_t>::max() || bottom > std::numeric_limits<std::int32_t>::max()) {
        return fail(error_code_e::arithmetic_overflow, "Desktop rectangle exceeds signed coordinate range", id);
      }
      if (rate.numerator == 0 || rate.denominator == 0) {
        return fail(error_code_e::invalid_refresh, "Refresh numerator and denominator must be nonzero", id);
      }
      if (layout.rotation != rotation_e::identity && layout.rotation != rotation_e::clockwise90 &&
          layout.rotation != rotation_e::clockwise180 && layout.rotation != rotation_e::clockwise270) {
        return fail(error_code_e::unsupported_rotation, "Rotation must be 0, 90, 180 or 270 degrees", id);
      }
      if ((layout.scale_percent && *layout.scale_percent == 0) ||
          (layout.hdr != hdr_state_e::unknown && layout.hdr != hdr_state_e::disabled && layout.hdr != hdr_state_e::enabled)) {
        return fail(error_code_e::invalid_geometry, "Invalid scale or HDR state", id);
      }
      if (size.width > limits.dimensions.width || size.height > limits.dimensions.height) {
        return fail(error_code_e::limit_exceeded, "Dimensions exceed the supplied policy", id);
      }
      const auto pixels = std::uint64_t(size.width) * size.height;
      if (pixels > std::numeric_limits<std::uint64_t>::max() / rate.numerator) {
        return fail(error_code_e::arithmetic_overflow, "Pixel-rate multiplication overflow", id);
      }
      const auto product = pixels * rate.numerator;
      const auto per_second = product / rate.denominator + (product % rate.denominator != 0);
      if (per_second > limits.pixels_per_second) {
        return fail(error_code_e::limit_exceeded, "Pixel rate exceeds the supplied per-monitor policy", id);
      }
      return per_second;
    }

    const observed_display_t *
    find_bound_display(const inventory_t &inventory, const resolution_result_t &result) {
      if (!result.binding) return nullptr;
      const auto it = std::find_if(inventory.displays.begin(), inventory.displays.end(),
        [&](const auto &display) { return display.id == result.binding->display; });
      return it == inventory.displays.end() ? nullptr : &*it;
    }

    bool
    cloned(const active_topology_t &groups, const os_display_id_t &a, const os_display_id_t &b) {
      return std::any_of(groups.begin(), groups.end(), [&](const auto &group) {
        return std::find(group.begin(), group.end(), a.value) != group.end() &&
               std::find(group.begin(), group.end(), b.value) != group.end();
      });
    }
  }  // namespace

  bool
  layout_t::operator==(const layout_t &other) const {
    return position == other.position && same_mode(mode, other.mode) && rotation == other.rotation &&
           scale_percent == other.scale_percent && hdr == other.hdr;
  }

  bool
  observed_display_t::operator==(const observed_display_t &other) const {
    return id == other.id && info.display_name == other.info.display_name &&
           info.friendly_name == other.info.friendly_name && info.device_state == other.info.device_state &&
           info.hdr_state == other.info.hdr_state && layout == other.layout && adapter == other.adapter &&
           incarnation == other.incarnation && head == other.head &&
           supported_modes.size() == other.supported_modes.size() &&
           std::equal(supported_modes.begin(), supported_modes.end(), other.supported_modes.begin(), same_mode);
  }

  bool
  limits_t::operator==(const limits_t &other) const {
    return monitors == other.monitors && virtual_monitors == other.virtual_monitors &&
           same_resolution(dimensions, other.dimensions) && pixels_per_second == other.pixels_per_second &&
           total_pixels_per_second == other.total_pixels_per_second;
  }

  bool
  legacy_intent_t::operator==(const legacy_intent_t &other) const {
    return output_name == other.output_name && keep_enabled == other.keep_enabled &&
           headless_create == other.headless_create && reuse == other.reuse &&
           parsed.device_id == other.parsed.device_id && parsed.device_prep == other.parsed.device_prep &&
           parsed.vdd_prep == other.parsed.vdd_prep && parsed.use_vdd == other.parsed.use_vdd &&
           parsed.change_hdr_state == other.parsed.change_hdr_state &&
           same_optional(parsed.resolution, other.parsed.resolution, same_resolution) &&
           same_optional(parsed.refresh_rate, other.parsed.refresh_rate, same_rate);
  }

  std::vector<resolution_result_t>
  resolve_profile(const profile_t &profile, const inventory_t &inventory) {
    std::vector<resolution_result_t> results;
    for (const auto &entry : profile.entries) {
      resolution_result_t result { .monitor = entry.id };
      const auto reject = [&](binding_state_e state, error_code_e code, std::string detail) {
        result.state = state;
        result.error = error_t { code, std::move(detail), entry.id };
      };
      const observed_head_t *head = nullptr;
      os_display_id_t target;
      std::optional<adapter_luid_t> expected_adapter;
      if (!inventory.available) {
        reject(binding_state_e::unavailable, error_code_e::inventory_unavailable, "Inventory observation failed");
      }
      else if (const auto *physical = std::get_if<physical_source_t>(&entry.source)) {
        target = physical->id;
        expected_adapter = physical->adapter;
        if (target.value.empty()) {
          reject(binding_state_e::needs_rebind, error_code_e::missing_identity, "Physical selection has no observed device ID");
        }
      }
      else if (const auto *virtual_source = std::get_if<virtual_source_t>(&entry.source)) {
        std::vector<const observed_head_t *> matches;
        for (const auto &candidate : inventory.heads) {
          if (candidate.identity.guid == virtual_source->guid) matches.push_back(&candidate);
        }
        if (matches.size() != 1) {
          reject(matches.empty() ? binding_state_e::unavailable : binding_state_e::needs_rebind,
            matches.empty() ? error_code_e::missing_identity : error_code_e::ambiguous_identity,
            "Virtual GUID must identify exactly one current head");
        }
        else {
          head = matches.front();
          if (!head->owner || *head->owner != virtual_source->owner) {
            reject(binding_state_e::unavailable, error_code_e::not_owned, "Virtual head ownership is not proven");
          }
          else if (head->adapter != virtual_source->adapter) {
            reject(binding_state_e::unavailable, error_code_e::adapter_mismatch, "Virtual producer adapter differs from requested adapter");
          }
          else if (!head->display || !valid_uuid(head->identity.driver) || head->identity.incarnation.value == 0) {
            reject(binding_state_e::needs_rebind, error_code_e::missing_identity, "Incomplete OS-to-head correlation");
          }
          else {
            target = *head->display;
            expected_adapter = virtual_source->adapter;
          }
        }
      }
      else {
        reject(binding_state_e::unavailable, error_code_e::missing_identity, "Legacy primary intent requires the P1 compatibility adapter");
      }

      if (!result.error) {
        std::vector<const observed_display_t *> matches;
        for (const auto &display : inventory.displays) {
          if (display.id == target) matches.push_back(&display);
        }
        if (matches.size() != 1) {
          reject(binding_state_e::needs_rebind,
            matches.empty() ? error_code_e::missing_identity : error_code_e::ambiguous_identity,
            "Observed display ID must identify exactly one device; administrator rebind required");
        }
        else {
          const auto &display = *matches.front();
          if (display.head.has_value() != (head != nullptr) || (head && *display.head != head->identity)) {
            reject(binding_state_e::needs_rebind, error_code_e::source_kind_mismatch, "OS display and full head identity do not agree");
          }
          else if (!display.adapter || (expected_adapter && *display.adapter != *expected_adapter)) {
            reject(binding_state_e::unavailable, error_code_e::adapter_mismatch, "Capture adapter is unknown or differs from source adapter");
          }
          else if (display.incarnation.value == 0 || display.info.device_state == device_state_e::inactive || !display.layout) {
            reject(binding_state_e::unavailable, error_code_e::unknown_applied_state, "Source is inactive or its applied state is unknown");
          }
          else {
            result.state = binding_state_e::bound;
            result.binding = source_binding_t { display.id, *display.adapter, display.incarnation, {} };
            if (head) result.binding->head = owned_head_t { head->identity, *head->owner, head->adapter, head->slot };
          }
        }
      }
      results.push_back(std::move(result));
    }
    return results;
  }

  result_t<void>
  validate_profile(const profile_t &profile, const inventory_t &inventory, topology_revision_t base, topology_revision_t current) {
    if (base != current) return fail(error_code_e::stale_revision, "Refresh the applied topology before updating");
    if (!inventory.available) return fail(error_code_e::inventory_unavailable, "Inventory observation failed");
    if (profile.version != 1) return fail(error_code_e::unsupported_version, "Unsupported profile version");
    if (!valid_uuid(profile.id) || profile.entries.empty() || profile.reconnect_grace.count() < 0) {
      return fail(error_code_e::invalid_profile, "Profile identity, selection or reconnect grace is invalid");
    }
    if (profile.entries.size() > profile.limits.monitors) return fail(error_code_e::limit_exceeded, "Monitor count exceeds policy");
    std::set<monitor_id_t> ids;
    std::set<os_display_id_t> physical_ids;
    std::set<head_guid_t> head_ids;
    for (const auto &entry : profile.entries) {
      if (!valid_uuid(entry.id)) return fail(error_code_e::invalid_identity, "Missing logical monitor UUID", entry.id);
      if (!ids.insert(entry.id).second) return fail(error_code_e::duplicate_monitor, "Duplicate logical monitor UUID", entry.id);
      if (const auto *physical = std::get_if<physical_source_t>(&entry.source)) {
        if (physical->id.value.empty()) return fail(error_code_e::missing_identity, "Physical ID is empty", entry.id);
        if (!physical_ids.insert(physical->id).second) return fail(error_code_e::duplicate_source, "Physical capture source selected twice", entry.id);
      }
      else if (const auto *head = std::get_if<virtual_source_t>(&entry.source)) {
        if (!valid_uuid(head->guid) || !valid_uuid(head->owner)) return fail(error_code_e::invalid_identity, "Missing virtual identity or owner", entry.id);
        if (!head_ids.insert(head->guid).second) return fail(error_code_e::duplicate_source, "Virtual head selected twice", entry.id);
      }
      else if (!profile.legacy) {
        return fail(error_code_e::invalid_profile, "Primary-by-default is only a legacy compatibility intent", entry.id);
      }
      if (entry.primary != primary_intent_e::preserve && entry.primary != primary_intent_e::make_primary) {
        return fail(error_code_e::invalid_profile, "Invalid primary intent", entry.id);
      }
    }
    if (head_ids.size() > profile.limits.virtual_monitors) return fail(error_code_e::limit_exceeded, "Virtual head count exceeds policy");
    const std::set<monitor_id_t> order(profile.order.begin(), profile.order.end());
    if (profile.order.size() != ids.size() || order != ids || !ids.contains(profile.fallback) || !ids.contains(profile.preferred)) {
      return fail(error_code_e::invalid_order, "Order must be a permutation; fallback and preferred must be selected");
    }

    const auto bindings = resolve_profile(profile, inventory);
    std::set<os_display_id_t> selected;
    std::set<os_display_id_t> edited;
    std::vector<std::pair<os_display_id_t, layout_t>> selected_layouts;
    std::uint64_t total_rate {};
    std::size_t primary_requests {};
    for (std::size_t i = 0; i < profile.entries.size(); ++i) {
      const auto &entry = profile.entries[i];
      if (bindings[i].error && (bindings[i].error->code == error_code_e::adapter_mismatch ||
                                 bindings[i].error->code == error_code_e::ambiguous_identity ||
                                 bindings[i].error->code == error_code_e::source_kind_mismatch ||
                                 bindings[i].error->code == error_code_e::not_owned)) {
        return std::unexpected(*bindings[i].error);
      }
      const auto *observed = find_bound_display(inventory, bindings[i]);
      if (observed && !selected.insert(observed->id).second) return fail(error_code_e::duplicate_source, "Two entries bind the same OS capture source", entry.id);
      const auto *layout = entry.desired ? &*entry.desired : (observed ? &*observed->layout : nullptr);
      if (layout) {
        const auto rate = validate_layout(*layout, profile.limits, entry.id);
        if (!rate) return std::unexpected(rate.error());
        if (*rate > std::numeric_limits<std::uint64_t>::max() - total_rate) return fail(error_code_e::arithmetic_overflow, "Aggregate pixel-rate overflow", entry.id);
        total_rate += *rate;
        selected_layouts.emplace_back(observed ? observed->id : os_display_id_t {}, *layout);
      }
      if (entry.primary == primary_intent_e::make_primary) {
        ++primary_requests;
        if (!profile.permissions.change_topology || !profile.permissions.primary ||
            (std::holds_alternative<physical_source_t>(entry.source) && !profile.permissions.change_physical)) {
          return fail(error_code_e::permission_denied, "OS primary changes require explicit permission", entry.id);
        }
        if (observed) edited.insert(observed->id);
      }
      if (entry.desired && (!observed || *entry.desired != *observed->layout)) {
        const auto &permissions = profile.permissions;
        const bool physical = std::holds_alternative<physical_source_t>(entry.source);
        if (!permissions.change_topology || (physical && !permissions.change_physical) ||
            (!physical && !observed && !permissions.create_virtual)) {
          return fail(error_code_e::permission_denied, "Host layout changes are not authorized", entry.id);
        }
        if (observed) {
          const auto &actual = *observed->layout;
          if ((!same_mode(entry.desired->mode, actual.mode) && !permissions.resize) ||
              (entry.desired->rotation != actual.rotation && !permissions.rotate) ||
              (entry.desired->hdr != actual.hdr && !permissions.hdr) ||
              (entry.desired->scale_percent != actual.scale_percent && !permissions.scale)) {
            return fail(error_code_e::permission_denied, "Requested mode, rotation, HDR or scale change is not authorized", entry.id);
          }
          if (!same_mode(entry.desired->mode, actual.mode) &&
              std::none_of(observed->supported_modes.begin(), observed->supported_modes.end(),
                [&](const auto &mode) { return same_mode(mode, entry.desired->mode); })) {
            return fail(error_code_e::unsupported_mode, "Requested mode is not in the supplied capability snapshot", entry.id);
          }
          edited.insert(observed->id);
        }
      }
    }
    if (total_rate > profile.limits.total_pixels_per_second) return fail(error_code_e::limit_exceeded, "Aggregate pixel rate exceeds policy");
    if (primary_requests > 1) return fail(error_code_e::invalid_profile, "Extended profile cannot request multiple OS primaries");

    for (const auto &group : inventory.clone_groups) {
      if (group.empty()) return fail(error_code_e::invalid_profile, "Empty clone group in inventory");
      const auto edited_count = std::count_if(group.begin(), group.end(), [&](const auto &id) { return edited.contains(os_display_id_t { id }); });
      if (edited_count != 0 && edited_count != static_cast<std::ptrdiff_t>(group.size())) {
        return fail(error_code_e::partial_clone_edit, "A clone group cannot be partially edited");
      }
      if (std::count_if(group.begin(), group.end(), [&](const auto &id) { return selected.contains(os_display_id_t { id }); }) > 1) {
        return fail(error_code_e::duplicate_source, "Cloned outputs are one capture source");
      }
    }
    for (std::size_t i = 0; i < selected_layouts.size(); ++i) {
      for (std::size_t j = i + 1; j < selected_layouts.size(); ++j) {
        if (overlaps(selected_layouts[i].second, selected_layouts[j].second)) {
          return fail(error_code_e::overlap, "Selected extended monitor rectangles overlap");
        }
      }
      for (const auto &unmanaged : inventory.displays) {
        if (selected.contains(unmanaged.id) || unmanaged.info.device_state == device_state_e::inactive) continue;
        if (!unmanaged.layout) return fail(error_code_e::unknown_applied_state, "Unmanaged active display geometry is unknown");
        if (overlaps(selected_layouts[i].second, *unmanaged.layout) &&
            !cloned(inventory.clone_groups, selected_layouts[i].first, unmanaged.id)) {
          return fail(error_code_e::overlap, "Selected layout overlaps an unmanaged display");
        }
      }
    }
    return {};
  }

  result_t<void>
  validate_applied(const requested_topology_t &requested, const applied_topology_t &applied) {
    if (requested.owner != applied.owner || requested.request != applied.request || requested.base != applied.base ||
        applied.revision.value <= applied.base.value) {
      return fail(error_code_e::stale_revision, "Applied record does not belong to this request and a newer revision");
    }
    if (applied.monitors.size() != requested.profile.entries.size()) return fail(error_code_e::invalid_profile, "Every requested entry needs an explicit applied outcome");
    std::set<monitor_id_t> seen;
    std::uint64_t total_rate {};
    for (const auto &monitor : applied.monitors) {
      const auto entry = std::find_if(requested.profile.entries.begin(), requested.profile.entries.end(),
        [&](const auto &candidate) { return candidate.id == monitor.monitor; });
      if (entry == requested.profile.entries.end() || !seen.insert(monitor.monitor).second) {
        return fail(error_code_e::duplicate_monitor, "Unknown or duplicate applied monitor", monitor.monitor);
      }
      if (monitor.state != applied_state_e::applied && monitor.state != applied_state_e::adjusted &&
          monitor.state != applied_state_e::unavailable && monitor.state != applied_state_e::conflict) {
        return fail(error_code_e::invalid_profile, "Unknown applied outcome", monitor.monitor);
      }
      if (monitor.state == applied_state_e::unavailable || monitor.state == applied_state_e::conflict) {
        if (monitor.reason.empty()) return fail(error_code_e::unknown_applied_state, "Unavailable/conflict outcome requires a reason", monitor.monitor);
        if (entry->required || !requested.profile.allow_degraded) return fail(error_code_e::permission_denied, "Required selection or unauthorized degradation", monitor.monitor);
      }
      else {
        if (!monitor.source || !monitor.layout || !monitor.host_primary.has_value()) return fail(error_code_e::unknown_applied_state, "Applied success requires actual source, layout and primary readback", monitor.monitor);
        const auto rate = validate_layout(*monitor.layout, requested.profile.limits, monitor.monitor);
        if (!rate) return std::unexpected(rate.error());
        if (*rate > std::numeric_limits<std::uint64_t>::max() - total_rate) return fail(error_code_e::arithmetic_overflow, "Applied aggregate pixel-rate overflow", monitor.monitor);
        total_rate += *rate;
        const auto &source = *monitor.source;
        if (source.display.value.empty() || source.incarnation.value == 0) return fail(error_code_e::invalid_identity, "Applied source needs a current observed identity", monitor.monitor);
        if (const auto *physical = std::get_if<physical_source_t>(&entry->source)) {
          if (source.display != physical->id || source.head) return fail(error_code_e::source_kind_mismatch, "Applied physical source differs from the selection", monitor.monitor);
          if (physical->adapter && source.adapter != *physical->adapter) return fail(error_code_e::adapter_mismatch, "Applied physical source changed adapter", monitor.monitor);
        }
        else if (const auto *virtual_source = std::get_if<virtual_source_t>(&entry->source)) {
          if (!source.head || source.head->identity.guid != virtual_source->guid) return fail(error_code_e::source_kind_mismatch, "Applied virtual head differs from the selected full GUID", monitor.monitor);
          if (source.adapter != virtual_source->adapter || source.head->adapter != source.adapter) return fail(error_code_e::adapter_mismatch, "Applied producer and capture adapters disagree", monitor.monitor);
          if (source.head->owner != virtual_source->owner || source.head->owner != requested.owner) return fail(error_code_e::not_owned, "Applied virtual head is not owned by the requesting principal", monitor.monitor);
          if (!valid_uuid(source.head->identity.driver) || source.head->identity.incarnation.value == 0) return fail(error_code_e::invalid_identity, "Applied head needs its current full runtime identity", monitor.monitor);
        }
        else {
          return fail(error_code_e::source_kind_mismatch, "Resolve legacy primary intent before publishing a native applied source", monitor.monitor);
        }
        const bool changed = entry->desired && *entry->desired != *monitor.layout;
        if ((changed || monitor.state == applied_state_e::adjusted) &&
            (!requested.profile.allow_adjustments || monitor.state != applied_state_e::adjusted || monitor.reason.empty())) {
          return fail(error_code_e::permission_denied, "Adjustment needs prior permission and an explicit reason", monitor.monitor);
        }
      }
    }
    if (total_rate > requested.profile.limits.total_pixels_per_second) return fail(error_code_e::limit_exceeded, "Applied aggregate pixel rate exceeds policy");
    return {};
  }

  result_t<request_receipt_t>
  request_history_t::remember(const requested_topology_t &request, topology_revision_t current) {
    const auto key = std::pair { request.owner, request.request };
    if (const auto it = records_.find(key); it != records_.end()) {
      if (it->second.requested != request) return fail(error_code_e::request_conflict, "Request ID was already used with different content");
      return it->second;
    }
    if (!valid_uuid(request.owner) || !valid_uuid(request.request)) return fail(error_code_e::invalid_identity, "Request requires owner and request UUIDs");
    if (request.base != current) return fail(error_code_e::stale_revision, "Request base is stale");
    if (records_.size() >= limit_) return fail(error_code_e::history_full, "Request history is full; records are never silently evicted");
    request_receipt_t receipt { records_.size() + 1, request, {} };
    records_.emplace(key, receipt);
    return receipt;
  }

  result_t<request_receipt_t>
  request_history_t::complete(const applied_topology_t &applied) {
    const auto it = records_.find({ applied.owner, applied.request });
    if (it == records_.end()) return fail(error_code_e::missing_identity, "Unknown request completion");
    if (it->second.applied) {
      if (*it->second.applied != applied) return fail(error_code_e::request_conflict, "Request already has a different applied result");
      return it->second;
    }
    const auto valid = validate_applied(it->second.requested, applied);
    if (!valid) return std::unexpected(valid.error());
    it->second.applied = applied;
    return it->second;
  }

  result_t<profile_t>
  synthesize_legacy(const legacy_intent_t &settings, profile_id_t profile_id,
    monitor_id_t monitor, head_guid_t head, owner_id_t owner, adapter_luid_t adapter, const limits_t &limits) {
    if (!valid_uuid(profile_id) || !valid_uuid(monitor)) return fail(error_code_e::invalid_identity, "Compatibility profile needs supplied durable IDs");
    profile_t profile;
    profile.id = profile_id;
    profile.name = "Legacy display";
    profile.order = { monitor };
    profile.preferred = monitor;
    profile.fallback = monitor;
    profile.limits = limits;
    profile.legacy = settings;
    monitor_entry_t entry { .id = monitor };
    if (settings.parsed.use_vdd.value_or(false)) {
      if (!valid_uuid(head) || !valid_uuid(owner)) return fail(error_code_e::invalid_identity, "Virtual legacy entry needs its own full GUID and owner");
      entry.source = virtual_source_t { head, owner, adapter };
    }
    else if (settings.output_name.empty()) {
      entry.source = legacy_primary_t {};
    }
    else {
      entry.source = physical_source_t { os_display_id_t { settings.output_name }, {} };
    }
    profile.entries.push_back(std::move(entry));
    return profile;
  }
}  // namespace display_device::topology
