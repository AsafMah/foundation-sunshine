#include "topology_journal.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace display_device::topology {
  namespace {
    using json = nlohmann::json;
    constexpr std::size_t max_bytes = 1024 * 1024;
    constexpr std::size_t max_records = 4096;

    struct schema_error_t: std::runtime_error {
      using std::runtime_error::runtime_error;
    };

    std::uint64_t
    integer(const json &value, std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max()) {
      if ((!value.is_number_unsigned() && !value.is_number_integer()) ||
          (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() < 0)) {
        throw schema_error_t("Expected a nonnegative integer");
      }
      const auto result = value.get<std::uint64_t>();
      if (result > maximum) throw schema_error_t("Integer exceeds field range");
      return result;
    }

    std::uint32_t
    u32(const json &value) {
      return static_cast<std::uint32_t>(integer(value, std::numeric_limits<std::uint32_t>::max()));
    }

    std::int32_t
    coordinate(const json &value) {
      if (!value.is_number_integer() ||
          (value.is_number_unsigned() && value.get<std::uint64_t>() > std::numeric_limits<std::int32_t>::max())) {
        throw schema_error_t("Expected a signed desktop coordinate");
      }
      const auto result = value.get<std::int64_t>();
      if (result < std::numeric_limits<std::int32_t>::min() || result > std::numeric_limits<std::int32_t>::max()) {
        throw schema_error_t("Desktop coordinate overflow");
      }
      return static_cast<std::int32_t>(result);
    }

    template <class Id>
    json
    uuid(const Id &id) {
      return id.value;
    }

    template <class Id>
    Id
    read_uuid(const json &value) {
      if (!value.is_array() || value.size() != 16) throw schema_error_t("Identity must contain all 128 bits");
      uuid_bytes_t bytes {};
      for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(integer(value[i], 255));
      Id id { bytes };
      if (!valid_uuid(id)) throw schema_error_t("Null identity is not a binding");
      return id;
    }

    template <class T, class Encode>
    json
    optional(const std::optional<T> &value, Encode encode) {
      return value ? encode(*value) : json(nullptr);
    }

    template <class T, class Decode>
    std::optional<T>
    read_optional(const json &value, Decode decode) {
      return value.is_null() ? std::nullopt : std::optional<T> { decode(value) };
    }

    void
    array(const json &value) {
      if (!value.is_array() || value.size() > max_records) throw schema_error_t("Invalid or excessive record count");
    }

    json
    mode(const display_mode_t &value) {
      return { { "width", value.resolution.width }, { "height", value.resolution.height },
        { "numerator", value.refresh_rate.numerator }, { "denominator", value.refresh_rate.denominator } };
    }

    display_mode_t
    read_mode(const json &value) {
      display_mode_t result { { u32(value.at("width")), u32(value.at("height")) },
        { u32(value.at("numerator")), u32(value.at("denominator")) } };
      if (result.resolution.width == 0 || result.resolution.height == 0 || result.refresh_rate.numerator == 0 || result.refresh_rate.denominator == 0) {
        throw schema_error_t("Invalid observed mode");
      }
      return result;
    }

    rotation_e
    read_rotation(const json &value) {
      const auto number = integer(value, 270);
      if (number % 90 != 0) throw schema_error_t("Invalid rotation");
      return static_cast<rotation_e>(number);
    }

    json
    position(const position_t &value) {
      return { { "x", value.x }, { "y", value.y } };
    }

    position_t
    read_position(const json &value) {
      return { coordinate(value.at("x")), coordinate(value.at("y")) };
    }

    hdr_state_e
    read_hdr(const json &value) {
      return static_cast<hdr_state_e>(integer(value, static_cast<std::uint64_t>(hdr_state_e::enabled)));
    }

    json
    layout(const layout_t &value) {
      return { { "position", position(value.position) }, { "mode", mode(value.mode) },
        { "rotation", value.rotation }, { "scale", value.scale_percent ? json(*value.scale_percent) : json(nullptr) },
        { "hdr", static_cast<int>(value.hdr) } };
    }

    layout_t
    read_layout(const json &value) {
      auto result = layout_t { read_position(value.at("position")), read_mode(value.at("mode")), read_rotation(value.at("rotation")),
        read_optional<std::uint32_t>(value.at("scale"), u32), read_hdr(value.at("hdr")) };
      if (result.scale_percent && *result.scale_percent == 0) throw schema_error_t("Zero scale is not known scale");
      return result;
    }

    json
    head_identity(const head_identity_t &value) {
      return { { "driver", uuid(value.driver) }, { "guid", uuid(value.guid) }, { "incarnation", value.incarnation.value } };
    }

    head_identity_t
    read_head_identity(const json &value) {
      head_identity_t result { read_uuid<driver_instance_id_t>(value.at("driver")), read_uuid<head_guid_t>(value.at("guid")),
        head_incarnation_t { integer(value.at("incarnation")) } };
      if (result.incarnation.value == 0) throw schema_error_t("Head incarnation is unknown");
      return result;
    }

    json
    owned_head(const owned_head_t &value) {
      return { { "identity", head_identity(value.identity) }, { "owner", uuid(value.owner) },
        { "adapter", value.adapter.value }, { "slot", value.slot } };
    }

    owned_head_t
    read_owned_head(const json &value) {
      return { read_head_identity(value.at("identity")), read_uuid<owner_id_t>(value.at("owner")),
        adapter_luid_t { integer(value.at("adapter")) }, u32(value.at("slot")) };
    }

    os_display_id_t
    read_display_id(const json &value) {
      auto id = value.get<std::string>();
      if (id.empty()) throw schema_error_t("Empty observed display ID");
      return os_display_id_t { std::move(id) };
    }

    json
    source(const source_binding_t &value) {
      return { { "display", value.display.value }, { "adapter", value.adapter.value }, { "incarnation", value.incarnation.value },
        { "head", optional(value.head, owned_head) } };
    }

    source_binding_t
    read_source(const json &value) {
      source_binding_t result { read_display_id(value.at("display")), adapter_luid_t { integer(value.at("adapter")) },
        source_incarnation_t { integer(value.at("incarnation")) }, read_optional<owned_head_t>(value.at("head"), read_owned_head) };
      if (result.incarnation.value == 0) throw schema_error_t("Source incarnation is unknown");
      if (result.head && result.head->adapter != result.adapter) throw schema_error_t("Managed source and head adapters disagree");
      return result;
    }

    json
    inventory(const inventory_t &value) {
      json displays = json::array(), heads = json::array(), drivers = json::array();
      for (const auto &display : value.displays) {
        json modes = json::array();
        for (const auto &supported : display.supported_modes) modes.push_back(mode(supported));
        displays.push_back({ { "id", display.id.value }, { "name", display.info.display_name }, { "friendly", display.info.friendly_name },
          { "state", static_cast<int>(display.info.device_state) }, { "hdr", static_cast<int>(display.info.hdr_state) },
          { "layout", optional(display.layout, layout) }, { "adapter", optional(display.adapter, [](auto adapter) { return json(adapter.value); }) },
          { "incarnation", display.incarnation.value }, { "head", optional(display.head, head_identity) }, { "modes", modes } });
      }
      for (const auto &head : value.heads) {
        heads.push_back({ { "identity", head_identity(head.identity) }, { "owner", optional(head.owner, uuid<owner_id_t>) },
          { "adapter", head.adapter.value }, { "slot", head.slot }, { "display", optional(head.display, [](const auto &id) { return json(id.value); }) } });
      }
      for (const auto &driver : value.drivers) {
        drivers.push_back({ { "driver", uuid(driver.driver) }, { "adapter", driver.adapter.value }, { "slots", driver.slots } });
      }
      return { { "revision", value.revision.value }, { "available", value.available }, { "displays", displays }, { "heads", heads },
        { "drivers", drivers }, { "cloneGroups", value.clone_groups },
        { "audioEndpoint", optional(value.audio_endpoint, [](const auto &endpoint) { return json(endpoint); }) } };
    }

    inventory_t
    read_inventory(const json &value) {
      inventory_t result;
      result.revision = inventory_revision_t { integer(value.at("revision")) };
      result.available = value.at("available").get<bool>();
      for (const auto *field : { "displays", "heads", "drivers", "cloneGroups" }) array(value.at(field));
      for (const auto &display : value.at("displays")) {
        observed_display_t record { read_display_id(display.at("id")),
          { display.at("name").get<std::string>(), display.at("friendly").get<std::string>(),
            static_cast<device_state_e>(integer(display.at("state"), static_cast<std::uint64_t>(device_state_e::primary))), read_hdr(display.at("hdr")) },
          read_optional<layout_t>(display.at("layout"), read_layout),
          read_optional<adapter_luid_t>(display.at("adapter"), [](const auto &item) { return adapter_luid_t { integer(item) }; }),
          source_incarnation_t { integer(display.at("incarnation")) },
          read_optional<head_identity_t>(display.at("head"), read_head_identity), {} };
        array(display.at("modes"));
        for (const auto &supported : display.at("modes")) record.supported_modes.push_back(read_mode(supported));
        result.displays.push_back(std::move(record));
      }
      for (const auto &head : value.at("heads")) {
        result.heads.push_back({ read_head_identity(head.at("identity")), read_optional<owner_id_t>(head.at("owner"), read_uuid<owner_id_t>),
          adapter_luid_t { integer(head.at("adapter")) }, u32(head.at("slot")), read_optional<os_display_id_t>(head.at("display"), read_display_id) });
      }
      for (const auto &driver : value.at("drivers")) {
        result.drivers.push_back({ read_uuid<driver_instance_id_t>(driver.at("driver")), adapter_luid_t { integer(driver.at("adapter")) }, u32(driver.at("slots")) });
      }
      for (const auto &group : value.at("cloneGroups")) {
        array(group);
        result.clone_groups.push_back(group.get<std::vector<std::string>>());
      }
      result.audio_endpoint = read_optional<std::string>(value.at("audioEndpoint"), [](const auto &item) { return item.template get<std::string>(); });
      return result;
    }

    json
    values(const managed_values_t &value) {
      return { { "position", optional(value.position, position) }, { "mode", optional(value.mode, mode) },
        { "rotation", optional(value.rotation, [](auto item) { return json(static_cast<int>(item)); }) },
        { "scale", optional(value.scale_percent, [](auto item) { return json(item); }) },
        { "hdr", optional(value.hdr, [](auto item) { return json(static_cast<int>(item)); }) },
        { "primary", optional(value.primary, [](auto item) { return json(item); }) } };
    }

    managed_values_t
    read_values(const json &value) {
      return { read_optional<position_t>(value.at("position"), read_position), read_optional<display_mode_t>(value.at("mode"), read_mode),
        read_optional<rotation_e>(value.at("rotation"), read_rotation), read_optional<std::uint32_t>(value.at("scale"), u32),
        read_optional<hdr_state_e>(value.at("hdr"), read_hdr),
        read_optional<bool>(value.at("primary"), [](const auto &item) { return item.template get<bool>(); }) };
    }

    json
    audio_change(const managed_audio_t &value) {
      return { { "baseline", value.baseline }, { "intended", value.intended },
        { "confirmed", optional(value.confirmed, [](const auto &endpoint) { return json(endpoint); }) } };
    }

    managed_audio_t
    read_audio_change(const json &value) {
      return { value.at("baseline").get<std::string>(), value.at("intended").get<std::string>(),
        read_optional<std::string>(value.at("confirmed"), [](const auto &item) { return item.template get<std::string>(); }) };
    }

    void
    validate(const journal_record_t &record) {
      if (!valid_uuid(record.owner) || !valid_uuid(record.transaction) || !valid_uuid(record.request)) throw schema_error_t("Missing journal authority/transaction/request identity");
      if (!record.baseline.available) throw schema_error_t("Baseline inventory failed");
      if (record.audio && record.baseline.audio_endpoint != record.audio->baseline) throw schema_error_t("Managed audio endpoint has no matching known baseline");
      std::set<os_display_id_t> baseline_ids;
      for (const auto &display : record.baseline.displays) {
        if (!baseline_ids.insert(display.id).second) throw schema_error_t("Ambiguous baseline display identity");
        if (display.info.device_state != device_state_e::inactive && !display.layout) throw schema_error_t("Active baseline geometry is unknown");
      }
      std::set<os_display_id_t> managed_ids;
      for (const auto &managed : record.managed) {
        if (!managed_ids.insert(managed.source.display).second || !baseline_ids.contains(managed.source.display)) throw schema_error_t("Managed display is duplicate or absent from baseline");
        const auto &display = *std::find_if(record.baseline.displays.begin(), record.baseline.displays.end(),
          [&](const auto &candidate) { return candidate.id == managed.source.display; });
        if (!display.layout || display.incarnation != managed.source.incarnation || display.adapter != managed.source.adapter ||
            display.head.has_value() != managed.source.head.has_value() ||
            (display.head && *display.head != managed.source.head->identity)) {
          throw schema_error_t("Managed runtime identity differs from the baseline");
        }
        const auto &actual = *display.layout;
        const managed_values_t actual_values { actual.position, actual.mode, actual.rotation, actual.scale_percent,
          actual.hdr == hdr_state_e::unknown ? std::nullopt : std::optional { actual.hdr },
          display.info.device_state == device_state_e::primary };
        const auto observed_values = values(actual_values);
        const auto intended = values(managed.intended), baseline = values(managed.baseline);
        const auto confirmed = managed.confirmed ? values(*managed.confirmed) : json::object();
        for (auto it = intended.begin(); it != intended.end(); ++it) {
          if (!baseline.at(it.key()).is_null() && baseline.at(it.key()) != observed_values.at(it.key())) throw schema_error_t("Managed baseline field differs from first observed state");
          if (!it.value().is_null() && baseline.at(it.key()).is_null()) throw schema_error_t("Managed field has no known baseline");
          if (confirmed.contains(it.key()) && !confirmed.at(it.key()).is_null() && it.value().is_null()) throw schema_error_t("Confirmed fields exceed managed intent");
        }
      }
      std::set<head_identity_t> heads;
      for (const auto &head : record.owned_heads) {
        if (head.owner != record.owner || !heads.insert(head.identity).second) throw schema_error_t("Journal cannot claim another owner's or duplicate head");
      }
      std::uint32_t previous {};
      for (const auto &step : record.steps) {
        if (step.sequence <= previous) throw schema_error_t("Journal steps must have increasing nonzero sequence numbers");
        previous = step.sequence;
        if (step.state == journal_step_state_e::observed && (!step.observed || !step.observed->available)) throw schema_error_t("Observed step requires successful readback");
        if ((step.state == journal_step_state_e::failed || step.state == journal_step_state_e::conflict) && step.detail.empty()) throw schema_error_t("Failure/conflict requires diagnostics");
        if ((step.state == journal_step_state_e::planned || step.state == journal_step_state_e::attempted) && step.observed) throw schema_error_t("Unobserved step cannot contain confirmed state");
      }
    }

    result_t<journal_record_t>
    decode(const json &value) {
      if (integer(value.at("version")) != 1) return std::unexpected(error_t { error_code_e::unsupported_version, "Unknown journal schema version; do not attempt recovery", {} });
      journal_record_t record;
      record.owner = read_uuid<owner_id_t>(value.at("owner"));
      record.transaction = read_uuid<transaction_id_t>(value.at("transaction"));
      record.request = read_uuid<request_id_t>(value.at("request"));
      record.base = topology_revision_t { integer(value.at("base")) };
      record.baseline = read_inventory(value.at("baseline"));
      record.audio = read_optional<managed_audio_t>(value.at("audio"), read_audio_change);
      for (const auto *field : { "managed", "ownedHeads", "steps" }) array(value.at(field));
      for (const auto &managed : value.at("managed")) {
        record.managed.push_back({ read_source(managed.at("source")), read_values(managed.at("baseline")), read_values(managed.at("intended")),
          read_optional<managed_values_t>(managed.at("confirmed"), read_values) });
      }
      for (const auto &head : value.at("ownedHeads")) record.owned_heads.push_back(read_owned_head(head));
      for (const auto &step : value.at("steps")) {
        record.steps.push_back({ u32(step.at("sequence")), static_cast<journal_operation_e>(integer(step.at("operation"), 3)),
          read_optional<os_display_id_t>(step.at("display"), read_display_id), read_optional<head_guid_t>(step.at("headIntent"), read_uuid<head_guid_t>),
          static_cast<journal_step_state_e>(integer(step.at("state"), 4)), read_optional<inventory_t>(step.at("observed"), read_inventory),
          step.at("detail").get<std::string>() });
      }
      validate(record);
      return record;
    }

    json
    encode(const journal_record_t &record) {
      json managed = json::array(), heads = json::array(), steps = json::array();
      for (const auto &display : record.managed) {
        managed.push_back({ { "source", source(display.source) }, { "baseline", values(display.baseline) },
          { "intended", values(display.intended) }, { "confirmed", optional(display.confirmed, values) } });
      }
      for (const auto &head : record.owned_heads) heads.push_back(owned_head(head));
      for (const auto &step : record.steps) {
        steps.push_back({ { "sequence", step.sequence }, { "operation", step.operation },
          { "display", optional(step.display, [](const auto &id) { return json(id.value); }) },
          { "headIntent", optional(step.head_intent, uuid<head_guid_t>) }, { "state", step.state },
          { "observed", optional(step.observed, inventory) }, { "detail", step.detail } });
      }
      return { { "version", record.version }, { "owner", uuid(record.owner) }, { "transaction", uuid(record.transaction) },
        { "request", uuid(record.request) }, { "base", record.base.value }, { "baseline", inventory(record.baseline) },
        { "managed", managed }, { "ownedHeads", heads }, { "steps", steps }, { "audio", optional(record.audio, audio_change) } };
    }
  }  // namespace

  bool
  managed_values_t::operator==(const managed_values_t &other) const {
    return values(*this) == values(other);
  }

  result_t<journal_record_t>
  prepare_journal(const std::optional<journal_record_t> &existing, owner_id_t owner,
    transaction_id_t transaction, request_id_t request, topology_revision_t base, const inventory_t &baseline) {
    if (existing) {
      if (existing->owner != owner || existing->transaction != transaction || existing->request != request || existing->base != base) {
        return std::unexpected(error_t { error_code_e::request_conflict, "Another transaction owns the first baseline", {} });
      }
      const auto valid = serialize_journal(*existing);
      if (!valid) return std::unexpected(valid.error());
      return *existing;
    }
    journal_record_t record { 1, owner, transaction, request, base, baseline, {}, {}, {} };
    const auto valid = serialize_journal(record);
    if (!valid) return std::unexpected(valid.error());
    return record;
  }

  result_t<std::string>
  serialize_journal(const journal_record_t &record) {
    try {
      const auto value = encode(record);
      const auto checked = decode(value);
      if (!checked) return std::unexpected(checked.error());
      auto serialized = value.dump();
      if (serialized.size() > max_bytes) return std::unexpected(error_t { error_code_e::invalid_journal, "Journal exceeds in-memory schema byte bound", {} });
      return serialized;
    }
    catch (const json::exception &error) {
      return std::unexpected(error_t { error_code_e::invalid_journal, error.what(), {} });
    }
    catch (const schema_error_t &error) {
      return std::unexpected(error_t { error_code_e::invalid_journal, error.what(), {} });
    }
  }

  result_t<journal_record_t>
  deserialize_journal(std::string_view serialized) {
    if (serialized.size() > max_bytes) return std::unexpected(error_t { error_code_e::invalid_journal, "Journal exceeds in-memory schema byte bound", {} });
    try {
      return decode(json::parse(serialized));
    }
    catch (const json::exception &error) {
      return std::unexpected(error_t { error_code_e::invalid_journal, error.what(), {} });
    }
    catch (const schema_error_t &error) {
      return std::unexpected(error_t { error_code_e::invalid_journal, error.what(), {} });
    }
  }
}  // namespace display_device::topology
