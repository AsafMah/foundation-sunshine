/**
 * @file src/display_device/topology_actor_startup.cpp
 * @brief Startup recovery of the durable external actor record.
 */
#include "topology_actor_startup.h"

#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace display_device::topology {
  namespace {
    /** The cleanup-pending record the startup continuation must discharge, if any. */
    struct owed_t {
      std::mutex mutex;
      std::optional<actor_record_t> record;
    };

    enum class step_e : std::uint8_t {
      resume,
      wait,
      block,
    };

    void
    report(const actor_startup_options_t &options, const std::string &line) noexcept {
      if (!options.report) return;
      try {
        options.report(line);
      }
      catch (...) {
      }
    }

    actor_recovery_t
    classify(const actor_startup_options_t &options) {
      try {
        return classify_actor_recovery(options.store->load(), options.current_boot, options.observe);
      }
      catch (const std::exception &e) {
        return { actor_recovery_e::blocked, std::nullopt, std::string { "Actor record inspection failed: " } + e.what() };
      }
      catch (...) {
        return { actor_recovery_e::blocked, std::nullopt, "Actor record inspection failed" };
      }
    }

    /** Replace; an ambiguous failure is reconciled by reloading. */
    bool
    write(actor_record_store_t &store, const actor_record_t &current, const actor_record_t &next) {
      if (store.replace(current, next)) return true;
      const auto loaded = store.load();
      return loaded.status == actor_load_status_e::loaded && loaded.record == next;
    }

    step_e
    resolve(const actor_startup_options_t &options, const actor_recovery_t &recovery, owed_t &owed) {
      switch (recovery.disposition) {
        case actor_recovery_e::none:
          return step_e::resume;
        case actor_recovery_e::finished:
          // A terminal record owes nothing; failing to retire it only refuses the next restart.
          if (const auto retired = options.store->retire(*recovery.record); !retired) {
            report(options, "Discharged actor record could not be retired: " + retired.error().detail);
          }
          return step_e::resume;
        case actor_recovery_e::cleanup_pending: {
          std::lock_guard lock { owed.mutex };
          owed.record = *recovery.record;
          return step_e::resume;
        }
        case actor_recovery_e::actor_settled: {
          const auto settled = settle_actor(*recovery.record, recovery.record->operation);
          if (!settled || !write(*options.store, *recovery.record, *settled)) {
            report(options, "Recorded actor has exited but its settlement could not be persisted; topology stays fenced");
            return step_e::block;
          }
          std::lock_guard lock { owed.mutex };
          owed.record = *settled;
          return step_e::resume;
        }
        case actor_recovery_e::actor_running:
          return step_e::wait;
        case actor_recovery_e::blocked:
        default:
          report(options, "External actor recovery is blocked; topology stays fenced until restart: " + recovery.detail);
          return step_e::block;
      }
    }

    step_e
    resolve_safely(const actor_startup_options_t &options, const actor_recovery_t &recovery, owed_t &owed) noexcept {
      try {
        return resolve(options, recovery, owed);
      }
      catch (const std::exception &e) {
        report(options, std::string { "External actor recovery failed; topology stays fenced: " } + e.what());
      }
      catch (...) {
        report(options, "External actor recovery failed; topology stays fenced");
      }
      return step_e::block;
    }

    /** After startup restored: complete the obligation, or hand it to the still-deferred legacy restore. */
    void
    discharge(const actor_startup_options_t &options, const actor_record_t &record) {
      try {
        const bool pending = options.restore_pending && options.restore_pending();
        const auto next = pending ? transfer_actor_cleanup(record, record.operation, legacy_restore_owner) :
                                    complete_actor_cleanup(record, record.operation);
        if (!next) {
          report(options, "Actor cleanup could not be discharged: " + next.error().detail);
          return;
        }
        if (!write(*options.store, record, *next)) {
          report(options, "Actor cleanup could not be persisted; it is retried at the next startup");
          return;
        }
        if (const auto retired = options.store->retire(*next); !retired) {
          report(options, "Discharged actor record could not be retired: " + retired.error().detail);
        }
      }
      catch (const std::exception &e) {
        report(options, std::string { "Actor cleanup discharge failed; it is retried at the next startup: " } + e.what());
      }
    }
  }  // namespace

  actor_recovery_e
  recover_startup_actor(coordinator_t &coordinator, const mutation_capability_t &capability, actor_startup_options_t options,
    command_body_t startup) {
    if (!startup || !options.store || !options.restore_pending) {
      throw std::invalid_argument { "Startup actor recovery needs a record store, the startup body and the deferral query" };
    }
    auto shared = std::make_shared<const actor_startup_options_t>(std::move(options));
    auto owed = std::make_shared<owed_t>();
    // Suspend before reading anything, so no failure below can fall through to normal operation.
    auto suspension = coordinator.suspend_startup_for_external_recovery(capability,
      [shared, owed, startup = std::move(startup)](const mutation_capability_t &startup_capability, const command_context_t &context) {
        // An exception keeps the record owing cleanup for the next startup.
        startup(startup_capability, context);
        std::optional<actor_record_t> record;
        {
          std::lock_guard lock { owed->mutex };
          record = owed->record;
        }
        if (record) discharge(*shared, *record);
      });

    const auto initial = classify(*shared);
    switch (resolve_safely(*shared, initial, *owed)) {
      case step_e::resume:
        suspension->settle();
        break;
      case step_e::block:
        break;
      case step_e::wait:
        try {
          auto watch = [shared, owed, suspension, adopted = initial.record->operation]() {
            bool resumable = false;
            while (suspension->waiting()) {
              std::this_thread::sleep_for(shared->poll_interval);
              if (!resumable) {
                const auto recovery = classify(*shared);
                if (!recovery.record || recovery.record->operation != adopted) {
                  report(*shared, "The adopted actor's record was lost or replaced while it ran; topology stays fenced until restart");
                  return;
                }
                const auto step = resolve_safely(*shared, recovery, *owed);
                if (step == step_e::block) return;
                resumable = step == step_e::resume;
              }
              if (resumable) {
                try {
                  suspension->settle();
                  return;
                }
                catch (const std::exception &e) {
                  // The coordinator keeps the obligation; settling again retries the resume.
                  report(*shared, std::string { "Startup could not resume after the actor settled; retrying: " } + e.what());
                }
              }
            }
          };
          if (shared->spawn) {
            shared->spawn(std::move(watch));
          }
          else {
            std::thread { std::move(watch) }.detach();
          }
        }
        catch (const std::exception &e) {
          report(*shared, std::string { "Recorded actor is running but cannot be watched; topology stays fenced: " } + e.what());
          return actor_recovery_e::blocked;
        }
        break;
    }
    return initial.disposition;
  }

}  // namespace display_device::topology
