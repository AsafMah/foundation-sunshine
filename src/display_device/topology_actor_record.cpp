/**
 * @file src/display_device/topology_actor_record.cpp
 * @brief Durable write-ahead record for one external topology actor and its cleanup.
 */
#include "topology_actor_record.h"

#include "topology_durable_file.h"

#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <utility>
#include <vector>

namespace display_device::topology {

  namespace {
    using json = nlohmann::json;

    constexpr std::uint32_t current_version = 1;
    constexpr std::size_t read_limit = actor_record_read_limit;
    constexpr std::string_view noun = "actor record";

    error_t
    invalid(std::string detail) {
      return error_t { error_code_e::invalid_journal, std::move(detail), {} };
    }

    error_t
    transition_error(const actor_record_t &record, std::string_view action) {
      return error_t { error_code_e::invalid_transition,
        std::string { "Cannot " } + std::string { action } + " an external actor record in stage " + std::string { to_string(record.stage) }, {} };
    }

    result_t<void>
    same_operation(const actor_record_t &record, std::uint64_t operation) {
      if (record.operation != operation) {
        return std::unexpected(error_t { error_code_e::stale_token, "The external actor record belongs to another operation", {} });
      }
      return {};
    }

    bool
    valid_receipt(const actor_receipt_t &receipt) {
      return receipt.pid != 0 && receipt.creation_time != 0;
    }

    /** The stage/receipt/destination combinations a durable record may have. */
    result_t<void>
    validate(const actor_record_t &record) {
      if (record.version != current_version) {
        return std::unexpected(error_t { error_code_e::unsupported_version, "Unsupported external actor record version", {} });
      }
      if (record.operation == 0) return std::unexpected(invalid("External actor record has no operation"));
      if (record.boot && record.boot->empty()) return std::unexpected(invalid("External actor record has an empty boot identity"));
      if (record.receipt && !valid_receipt(*record.receipt)) return std::unexpected(invalid("External actor receipt is incomplete"));
      switch (record.stage) {
        case actor_stage_e::intent:
          if (record.receipt) return std::unexpected(invalid("An intent cannot carry a receipt"));
          break;
        case actor_stage_e::launched:
          if (!record.receipt) return std::unexpected(invalid("A launched actor needs a receipt"));
          break;
        case actor_stage_e::cleanup_pending:
        case actor_stage_e::completed:
          break;
        case actor_stage_e::transferred:
          if (record.transferred_to.empty()) return std::unexpected(invalid("A transferred obligation needs a destination"));
          return {};
        default:
          return std::unexpected(invalid("Unknown external actor stage"));
      }
      if (!record.transferred_to.empty()) return std::unexpected(invalid("Only a transferred obligation has a destination"));
      return {};
    }

    std::optional<actor_stage_e>
    parse_stage(std::string_view text) {
      for (const auto stage : { actor_stage_e::intent, actor_stage_e::launched, actor_stage_e::cleanup_pending,
             actor_stage_e::completed, actor_stage_e::transferred }) {
        if (to_string(stage) == text) return stage;
      }
      return std::nullopt;
    }

    std::string_view
    to_string(actor_obligation_e obligation) {
      switch (obligation) {
        case actor_obligation_e::restore_legacy_display: return "restore_legacy_display";
      }
      return "unknown";
    }

    std::optional<actor_obligation_e>
    parse_obligation(std::string_view text) {
      if (text == "restore_legacy_display") return actor_obligation_e::restore_legacy_display;
      return std::nullopt;
    }

    std::uint64_t
    integer(const json &value, std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max()) {
      if (!value.is_number_unsigned()) throw std::invalid_argument { "expected an unsigned integer" };
      const auto number = value.get<std::uint64_t>();
      if (number > maximum) throw std::invalid_argument { "integer out of range" };
      return number;
    }

    const json &
    field(const json &object, const char *name) {
      const auto found = object.find(name);
      if (found == object.end()) throw std::invalid_argument { std::string { "missing field " } + name };
      return *found;
    }

    std::string
    text(const json &value) {
      if (!value.is_string()) throw std::invalid_argument { "expected a string" };
      return value.get<std::string>();
    }

    actor_load_t
    to_load(durable_read_t read) {
      if (read.status == durable_read_e::absent) return {};
      if (read.status == durable_read_e::unreadable) {
        return { actor_load_status_e::unreadable, std::nullopt, std::move(read.detail) };
      }
      if (read.bytes.size() > read_limit) {
        return { actor_load_status_e::invalid, std::nullopt, "The external actor record is larger than any valid record" };
      }
      auto record = deserialize_actor_record(read.bytes);
      if (!record) {
        return { record.error().code == error_code_e::unsupported_version ? actor_load_status_e::unsupported_version : actor_load_status_e::invalid,
          std::nullopt, record.error().detail };
      }
      return { actor_load_status_e::loaded, std::move(*record), {} };
    }

    error_t
    store_error(std::string detail) {
      return error_t { error_code_e::invalid_transition, std::move(detail), {} };
    }

    result_t<void>
    write(const std::filesystem::path &path, const actor_record_t &record) {
      auto bytes = serialize_actor_record(record);
      if (!bytes) return std::unexpected(bytes.error());
      return replace_file_durably(path, *bytes, noun);
    }

    /** The only durable edges; identity, obligation and any receipt are immutable once written. */
    result_t<void>
    legal_successor(const actor_record_t &current, const actor_record_t &next) {
      if (auto valid = validate(next); !valid) return std::unexpected(valid.error());
      if (next.version != current.version || next.operation != current.operation || next.boot != current.boot ||
          next.obligation != current.obligation || next.obligation_detail != current.obligation_detail) {
        return std::unexpected(store_error("A replacement cannot change the external actor's identity or obligation"));
      }
      const auto from = current.stage;
      const auto to = next.stage;
      const bool edge = (from == actor_stage_e::intent && to == actor_stage_e::launched) ||
                        ((from == actor_stage_e::intent || from == actor_stage_e::launched) && to == actor_stage_e::cleanup_pending) ||
                        (from == actor_stage_e::cleanup_pending && (to == actor_stage_e::completed || to == actor_stage_e::transferred));
      if (!edge) return std::unexpected(transition_error(current, std::string { "move to " } + std::string { to_string(to) }));
      if (to != actor_stage_e::launched && next.receipt != current.receipt) {
        return std::unexpected(store_error("Only launching may record the external actor's receipt"));
      }
      return {};
    }
  }  // namespace

  std::string_view
  to_string(actor_stage_e stage) noexcept {
    switch (stage) {
      case actor_stage_e::intent: return "intent";
      case actor_stage_e::launched: return "launched";
      case actor_stage_e::cleanup_pending: return "cleanup_pending";
      case actor_stage_e::completed: return "completed";
      case actor_stage_e::transferred: return "transferred";
    }
    return "unknown";
  }

  std::string_view
  to_string(actor_recovery_e disposition) noexcept {
    switch (disposition) {
      case actor_recovery_e::none: return "none";
      case actor_recovery_e::blocked: return "blocked";
      case actor_recovery_e::actor_running: return "actor_running";
      case actor_recovery_e::actor_settled: return "actor_settled";
      case actor_recovery_e::cleanup_pending: return "cleanup_pending";
      case actor_recovery_e::finished: return "finished";
    }
    return "unknown";
  }

  bool
  actor_record_terminal(const actor_record_t &record) noexcept {
    return record.stage == actor_stage_e::completed || record.stage == actor_stage_e::transferred;
  }

  result_t<actor_record_t>
  begin_actor_intent(std::uint64_t operation, std::optional<std::string> boot, actor_obligation_e obligation, std::string detail) {
    actor_record_t record { current_version, operation, std::move(boot), obligation, std::move(detail), actor_stage_e::intent, std::nullopt, {} };
    if (auto valid = validate(record); !valid) return std::unexpected(valid.error());
    return record;
  }

  result_t<actor_record_t>
  record_actor_receipt(const actor_record_t &record, std::uint64_t operation, actor_receipt_t receipt) {
    if (auto same = same_operation(record, operation); !same) return std::unexpected(same.error());
    if (record.stage != actor_stage_e::intent) return std::unexpected(transition_error(record, "record a receipt for"));
    if (!valid_receipt(receipt)) return std::unexpected(invalid("External actor receipt is incomplete"));
    auto next = record;
    next.stage = actor_stage_e::launched;
    next.receipt = receipt;
    return next;
  }

  result_t<actor_record_t>
  settle_actor(const actor_record_t &record, std::uint64_t operation) {
    if (auto same = same_operation(record, operation); !same) return std::unexpected(same.error());
    if (record.stage != actor_stage_e::intent && record.stage != actor_stage_e::launched) {
      return std::unexpected(transition_error(record, "settle"));
    }
    auto next = record;
    next.stage = actor_stage_e::cleanup_pending;
    return next;
  }

  result_t<actor_record_t>
  complete_actor_cleanup(const actor_record_t &record, std::uint64_t operation) {
    if (auto same = same_operation(record, operation); !same) return std::unexpected(same.error());
    if (record.stage != actor_stage_e::cleanup_pending) return std::unexpected(transition_error(record, "complete cleanup for"));
    auto next = record;
    next.stage = actor_stage_e::completed;
    return next;
  }

  result_t<actor_record_t>
  transfer_actor_cleanup(const actor_record_t &record, std::uint64_t operation, std::string destination) {
    if (auto same = same_operation(record, operation); !same) return std::unexpected(same.error());
    if (record.stage != actor_stage_e::cleanup_pending) return std::unexpected(transition_error(record, "transfer cleanup for"));
    if (destination.empty()) return std::unexpected(invalid("A transferred obligation needs a destination"));
    auto next = record;
    next.stage = actor_stage_e::transferred;
    next.transferred_to = std::move(destination);
    return next;
  }

  result_t<std::string>
  serialize_actor_record(const actor_record_t &record) {
    if (auto valid = validate(record); !valid) return std::unexpected(valid.error());
    json value {
      { "version", record.version },
      { "operation", record.operation },
      { "boot", record.boot ? json(*record.boot) : json(nullptr) },
      { "obligation", to_string(record.obligation) },
      { "obligation_detail", record.obligation_detail },
      { "stage", to_string(record.stage) },
      { "receipt", record.receipt ? json { { "pid", record.receipt->pid }, { "creation_time", record.receipt->creation_time } } : json(nullptr) },
      { "transferred_to", record.transferred_to },
    };
    return value.dump();
  }

  result_t<actor_record_t>
  deserialize_actor_record(std::string_view serialized) {
    try {
      // nlohmann keeps the last of duplicate names; an ambiguous record must not be interpreted.
      std::vector<std::set<std::string>> keys;
      bool duplicate = false;
      const auto value = json::parse(serialized, [&](int, json::parse_event_t event, json &parsed) {
        if (event == json::parse_event_t::object_start) keys.emplace_back();
        else if (event == json::parse_event_t::object_end) keys.pop_back();
        else if (event == json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second) duplicate = true;
        return true;
      });
      if (duplicate) return std::unexpected(invalid("External actor record repeats a field"));
      if (!value.is_object()) return std::unexpected(invalid("External actor record is not an object"));
      if (value.size() != 8) return std::unexpected(invalid("External actor record has unexpected fields"));
      actor_record_t record;
      record.version = static_cast<std::uint32_t>(integer(field(value, "version"), std::numeric_limits<std::uint32_t>::max()));
      if (record.version != current_version) {
        return std::unexpected(error_t { error_code_e::unsupported_version, "Unsupported external actor record version", {} });
      }
      record.operation = integer(field(value, "operation"));
      const auto &boot = field(value, "boot");
      if (!boot.is_null()) record.boot = text(boot);
      const auto obligation = parse_obligation(text(field(value, "obligation")));
      if (!obligation) return std::unexpected(invalid("Unknown external actor obligation"));
      record.obligation = *obligation;
      record.obligation_detail = text(field(value, "obligation_detail"));
      const auto stage = parse_stage(text(field(value, "stage")));
      if (!stage) return std::unexpected(invalid("Unknown external actor stage"));
      record.stage = *stage;
      const auto &receipt = field(value, "receipt");
      if (!receipt.is_null()) {
        if (!receipt.is_object()) return std::unexpected(invalid("External actor receipt is not an object"));
        if (receipt.size() != 2) return std::unexpected(invalid("External actor receipt has unexpected fields"));
        record.receipt = actor_receipt_t {
          static_cast<std::uint32_t>(integer(field(receipt, "pid"), std::numeric_limits<std::uint32_t>::max())),
          integer(field(receipt, "creation_time")),
        };
      }
      record.transferred_to = text(field(value, "transferred_to"));
      if (auto valid = validate(record); !valid) {
        if (valid.error().code == error_code_e::unsupported_version) return std::unexpected(valid.error());
        return std::unexpected(invalid(valid.error().detail));
      }
      return record;
    }
    catch (const std::exception &error) {
      return std::unexpected(invalid(std::string { "Malformed external actor record: " } + error.what()));
    }
  }

  std::shared_ptr<std::mutex>
  actor_record_path_lock(const std::filesystem::path &path) {
    static std::mutex registry_mutex;
    static std::map<std::filesystem::path, std::shared_ptr<std::mutex>> registry;
    std::error_code error;
    auto key = std::filesystem::weakly_canonical(path, error);
    if (error) {
      key = std::filesystem::absolute(path, error);
      key = error ? path.lexically_normal() : key.lexically_normal();
    }
    std::lock_guard lock { registry_mutex };
    auto &slot = registry[key];
    if (!slot) slot = std::make_shared<std::mutex>();
    return slot;
  }

  file_actor_record_store_t::file_actor_record_store_t(std::filesystem::path path):
      path_(std::move(path)),
      mutex_(actor_record_path_lock(path_)) {}

  actor_load_t
  file_actor_record_store_t::load_locked() {
    return to_load(read_file_bounded(path_, read_limit + 1, noun));
  }

  actor_load_t
  file_actor_record_store_t::load() {
    std::lock_guard lock { *mutex_ };
    return load_locked();
  }

  result_t<void>
  file_actor_record_store_t::create(const actor_record_t &record) {
    std::lock_guard lock { *mutex_ };
    if (record.stage != actor_stage_e::intent) return std::unexpected(store_error("A new external actor record must start as intent"));
    if (load_locked().status != actor_load_status_e::absent) {
      return std::unexpected(store_error("An external actor record already exists"));
    }
    return write(path_, record);
  }

  result_t<void>
  file_actor_record_store_t::replace(const actor_record_t &expected, const actor_record_t &next) {
    std::lock_guard lock { *mutex_ };
    if (next.operation != expected.operation) return std::unexpected(store_error("A replacement must keep the operation"));
    if (auto legal = legal_successor(expected, next); !legal) return std::unexpected(legal.error());
    const auto current = load_locked();
    if (current.status != actor_load_status_e::loaded || current.record != expected) {
      return std::unexpected(store_error("The durable external actor record is not the expected record"));
    }
    return write(path_, next);
  }

  result_t<void>
  file_actor_record_store_t::retire(const actor_record_t &expected) {
    std::lock_guard lock { *mutex_ };
    if (!actor_record_terminal(expected)) return std::unexpected(store_error("Only a discharged external actor record can be retired"));
    const auto current = load_locked();
    if (current.status != actor_load_status_e::loaded || current.record != expected) {
      return std::unexpected(store_error("The durable external actor record is not the expected record"));
    }
    return remove_file_durably(path_, noun);
  }

  actor_recovery_t
  classify_actor_recovery(const actor_load_t &loaded, const std::optional<std::string> &current_boot, const process_observer_t &observe) {
    switch (loaded.status) {
      case actor_load_status_e::absent:
        return {};
      case actor_load_status_e::loaded:
        break;
      default:
        return { actor_recovery_e::blocked, std::nullopt, loaded.detail.empty() ? "Unusable external actor record" : loaded.detail };
    }
    if (!loaded.record) return { actor_recovery_e::blocked, std::nullopt, "Loaded external actor record is missing" };
    const auto &record = *loaded.record;
    if (auto valid = validate(record); !valid) return { actor_recovery_e::blocked, record, valid.error().detail };
    switch (record.stage) {
      case actor_stage_e::completed:
      case actor_stage_e::transferred:
        return { actor_recovery_e::finished, record, {} };
      case actor_stage_e::cleanup_pending:
        return { actor_recovery_e::cleanup_pending, record, {} };
      case actor_stage_e::intent:
      case actor_stage_e::launched:
        break;
    }
    // Only two known, different boot identities prove the actor cannot still run.
    if (record.boot && current_boot && *record.boot != *current_boot) {
      return { actor_recovery_e::actor_settled, record, "The boot changed since the actor was launched" };
    }
    if (record.stage == actor_stage_e::intent) {
      return { actor_recovery_e::blocked, record, "The actor may have been created before its receipt was recorded" };
    }
    if (!observe) return { actor_recovery_e::blocked, record, "No process observer" };
    process_observation_e observation;
    try {
      observation = observe(*record.receipt);
    }
    catch (const std::exception &error) {
      return { actor_recovery_e::blocked, record, std::string { "Cannot observe the actor: " } + error.what() };
    }
    catch (...) {
      return { actor_recovery_e::blocked, record, "Cannot observe the actor" };
    }
    switch (observation) {
      case process_observation_e::running:
        return { actor_recovery_e::actor_running, record, {} };
      case process_observation_e::exited:
      case process_observation_e::absent:
      case process_observation_e::identity_mismatch:
        return { actor_recovery_e::actor_settled, record, {} };
      case process_observation_e::inaccessible:
        break;
    }
    return { actor_recovery_e::blocked, record, "The actor's process could not be identified" };
  }

}  // namespace display_device::topology