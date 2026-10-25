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

    using signature_t = std::vector<std::pair<std::string, device_state_e>>;

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
        return step.state == journal_step_state_e::conflict;
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
    if (const auto key = unknown_key(monitor, { "source", "display" })) {
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
      return std::optional { native_profile_t { native_profile_t::source_e::virtual_head, {} } };
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
        reconcile(*loaded.record, false);
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
      return profile.source == native_profile_t::source_e::virtual_head ? prepare_virtual() :
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
    return { native_verdict_e::ok, native_target_t { display, false }, {} };
  }

  native_outcome_t
  native_executor_t::prepare_virtual() {
    const auto baseline = platform_.inventory();
    if (!baseline.available) {
      return refused("The display inventory is unavailable");
    }
    if (!virtual_heads(platform_, baseline).empty()) {
      return refused("A virtual display already exists; this version creates one only when none exists");
    }

    const transaction_id_t transaction { fresh_uuid() };
    const request_id_t request { fresh_uuid() };
    std::vector<head_guid_t> intents;
    for (std::uint32_t attempt = 0; attempt < options_.create_attempts; ++attempt) {
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

    std::string failure;
    for (const auto &intent : intents) {
      record.steps.push_back({ next_sequence(record), journal_operation_e::create_head, std::nullopt, intent,
        journal_step_state_e::attempted, std::nullopt, {} });
      // Write-ahead: the attempt is durable before the driver is asked.
      if (const auto saved = store_.save(record); !saved) {
        return clear_journal(refusal("The native journal could not be saved: " + saved.error().detail));
      }

      const auto created = platform_.create_head(intent);
      const auto after = platform_.inventory();
      if (!after.available) {
        return unresolved("The display inventory is unavailable after asking for a virtual display");
      }
      const auto heads = virtual_heads(platform_, after);
      if (heads.size() > 1) {
        return conflict("More than one virtual display appeared, so none can be proven to be this host's");
      }
      if (heads.empty()) {
        if (created) {
          return unresolved("The driver accepted the request but no virtual display appeared");
        }
        // The driver refused the request, so no display of it can appear later.
        failure = created.error().detail.empty() ? std::string { "the driver reported an error" } : created.error().detail;
        auto &step = record.steps.back();
        step.state = journal_step_state_e::failed;
        step.detail = failure;
        if (unmanaged_signature(platform_, after, record) != expected) {
          return clear_journal(refusal("Other displays changed while creating a virtual display: " + failure));
        }
        continue;
      }

      const auto &head = *heads.front();
      if (!created) {
        return roll_back_head(record, "The driver reported an error but a virtual display appeared: " + created.error().detail);
      }
      if (unmanaged_signature(platform_, after, record) != expected) {
        return roll_back_head(record, "Creating the virtual display changed other displays");
      }
      auto &step = record.steps.back();
      step.state = journal_step_state_e::observed;
      step.display = head.id;
      step.observed = after;
      record.owned_heads.push_back({ head_identity_t { options_.driver, intent, head_incarnation_t { 1 } }, owner_,
        head.adapter.value_or(adapter_luid_t {}), 0 });
      if (const auto saved = store_.save(record); !saved) {
        return roll_back_head(record, "The native journal could not record the new virtual display: " + saved.error().detail);
      }
      created_in_this_run_ = record.transaction;
      health_ = { true, false, {} };
      return { native_verdict_e::ok, native_target_t { head.id, true }, {} };
    }
    return clear_journal(refusal("No virtual display was created: " + failure));
  }

  native_outcome_t
  native_executor_t::roll_back_head(const journal_record_t &record, const std::string &why) {
    // Same-run proof: no virtual display existed before, exactly one exists now, and
    // the coordinator serializes every other virtual display operation.
    if (const auto removed = platform_.remove_all_heads(); !removed) {
      return conflict(why + "; removing it failed: " + removed.error().detail);
    }
    const auto after = platform_.inventory();
    if (!after.available || !virtual_heads(platform_, after).empty()) {
      return conflict(why + "; its removal could not be confirmed");
    }
    if (unmanaged_signature(platform_, after, record) != unmanaged_signature(platform_, record.baseline, record)) {
      return conflict(why + "; other displays still differ from the baseline after removing it");
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
      return reconcile(record, record.owner == owner_ && created_in_this_run_ == record.transaction);
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

  native_outcome_t
  native_executor_t::reconcile(const journal_record_t &record, bool same_run) {
    if (const auto problem = semantic_problem(record)) {
      return conflict("The native journal cannot be reconciled: " + *problem);
    }
    if (const auto *step = unresolved_step(record)) {
      return conflict("An earlier native operation was left unresolved (" + step->detail + "); check the host, then reset");
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

    std::optional<os_display_id_t> owned_display;
    if (attempted(record, journal_operation_e::create_head, nullptr)) {
      const auto heads = virtual_heads(platform_, current);
      if (!heads.empty()) {
        if (!same_run) {
          return conflict("A virtual display from an earlier run cannot be proven to be this host's, so it is left in place");
        }
        const auto *created = observed_create(record);
        if (record.owned_heads.size() != 1 || heads.size() != 1 || !created || created->display != heads.front()->id) {
          return conflict("The virtual displays present no longer match the one this host created, so none is removed");
        }
        owned_display = heads.front()->id;
      }
    }

    for (const auto &[id, values] : restores) {
      if (const auto applied = platform_.apply_fields(id, values); !applied) {
        return conflict("Restoring display " + id.value + " failed: " + applied.error().detail);
      }
    }
    if (owned_display) {
      if (!restores.empty()) {
        // The restores changed the topology, so the proof is repeated before removing.
        const auto before = platform_.inventory();
        const auto heads = before.available ? virtual_heads(platform_, before) : std::vector<const observed_display_t *> {};
        if (heads.size() != 1 || heads.front()->id != *owned_display) {
          return conflict("The virtual display could not be proven to be this host's after restoring fields, so it is left in place");
        }
      }
      if (const auto removed = platform_.remove_all_heads(); !removed) {
        return conflict("Removing the virtual display failed: " + removed.error().detail);
      }
    }
    if (restores.empty() && !owned_display) {
      // Nothing was written, so the first observation is the readback.
      if (unmanaged_signature(platform_, current, record) != unmanaged_signature(platform_, record.baseline, record)) {
        return conflict("Other displays differ from the baseline, so the native journal is kept");
      }
      return clear_journal({});
    }

    const auto after = platform_.inventory();
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
    if (owned_display && !virtual_heads(platform_, after).empty()) {
      return conflict("The virtual display is still present after its removal");
    }
    if (unmanaged_signature(platform_, after, record) != unmanaged_signature(platform_, record.baseline, record)) {
      return conflict("Other displays differ from the baseline after restoring");
    }
    return clear_journal({});
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

}  // namespace display_device::topology
