#pragma once

// standard includes
#include <mutex>
#include <atomic>
#include <optional>
#include <string>
// lib includes
#include <boost/atomic.hpp>
// local includes
#include "settings.h"
#include "vdd_utils.h"

namespace display_device {

  /**
   * @brief A singleton class for managing the display device configuration for the whole Sunshine session.
   *
   * This class is meant to be an entry point for applying the configuration and reverting it later
   * from within the various places in the Sunshine's source code.
   *
   * It is similar to settings_t and is more or less a wrapper around it.
   * However, this class ensures thread-safe usage for the methods and additionally
   * performs automatic cleanups.
   *
   * @note A lazy-evaluated, correctly-destroyed, thread-safe singleton pattern is used here (https://stackoverflow.com/a/1008289).
   */
  class session_t {
  public:
    /**
     * @brief A class that uses RAII to perform cleanup when it's destroyed.
     * @note The deinit_t usage pattern is used here instead of the session_t destructor
     *       to expedite the cleanup process in case of Sunshine termination.
     * @see session_t::init()
     */
    class deinit_t {
    public:
      /**
       * @brief A destructor that restores (or tries to) the initial state.
       */
      virtual ~deinit_t();
    };

    /**
     * @brief Get the singleton instance.
     * @returns Singleton instance for the class.
     *
     * EXAMPLES:
     * ```cpp
     * session_t& session { session_t::get() };
     * ```
     */
    static session_t &
    get();

    /**
     * @brief Initialize the singleton and perform the initial state recovery (if needed).
     * @returns A deinit_t instance that performs cleanup when destroyed.
     * @see deinit_t
     *
     * EXAMPLES:
     * ```cpp
     * const auto session_guard { session_t::init() };
     * ```
     */
    static std::unique_ptr<deinit_t>
    init();

    /**
     * @brief Result of trying to prepare display settings for a stream.
     */
    struct configure_result_t {
      enum class result_e {
        success,
        deferred_retry,
        vdd_not_installed,
        vdd_unavailable,
        vdd_create_failed,
        parse_fail,
        topology_fail,
        primary_display_fail,
        modes_fail,
        hdr_states_fail,
        file_save_fail,
        revert_fail
      };

      explicit
      operator bool() const {
        return result == result_e::success || result == result_e::deferred_retry;
      }

      result_e result;
      std::string message;
      std::string hint;
      bool cleanup_on_failure { false };
    };

    /**
     * @brief Configure the display device based on the user configuration and the session information.
     *
     * Upon failing to completely apply configuration, the applied settings will be reverted.
     * Or, in some cases, we will keep retrying even when the stream has already started as there
     * is no possibility to apply settings before the stream start.
     *
     * @param config User's video related configuration.
     * @param session Session information.
     * @returns A result describing whether the display configuration was applied, deferred,
     *          or failed. Callers may continue with the stream when appropriate.
     *
     * EXAMPLES:
     * ```cpp
     * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session; // Assuming ptr is properly initialized
     * const config::video_t &video_config { config::video };
     *
     * const auto result = session_t::get().configure_display(video_config, *launch_session);
     * ```
     */
    configure_result_t
    configure_display(const topology::mutation_capability_t &capability, const config::video_t &config,
      const rtsp_stream::launch_session_t &session, automatic_vdd_policy_e vdd_policy, bool is_reconfigure = false);

    /**
     * @brief Revert the display configuration and restore the previous state.
     * @note This method automatically loads the persistence (if any) from the previous Sunshine session.
     * @note In case the state could not be restored, it will be retried again in X seconds
     *       (repeating indefinitely until success or until persistence is reset).
     *
     * EXAMPLES:
     * ```cpp
     * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session; // Assuming ptr is properly initialized
     * const config::video_t &video_config { config::video };
     *
     * const auto result = session_t::get().configure_display(video_config, *launch_session);
     * if (result) {
     *   // Wait for some time
     *   session_t::get().restore_state();
     * }
     * ```
     */
    void
    restore_state(const topology::mutation_capability_t &capability, automatic_vdd_policy_e vdd_policy);

    bool restore_pending() const noexcept { return pending_restore_.load(); }

    /**
     * @brief Reset the persistence and currently held initial display state.
     *
     * This is normally used to get out of the "broken" state where the algorithm wants
     * to restore the initial display state and refuses start the stream in most cases.
     *
     * This could happen if the display is no longer available or the hardware was changed
     * and the device ids no longer match.
     *
     * The user then accepts that Sunshine is not able to restore the state and "agrees" to
     * do it manually.
     *
     * @note This also stops the retry timer.
     *
     * EXAMPLES:
     * ```cpp
     * const std::shared_ptr<rtsp_stream::launch_session_t> launch_session; // Assuming ptr is properly initialized
     * const config::video_t &video_config { config::video };
     *
     * const auto result = session_t::get().configure_display(video_config, *launch_session);
     * if (!result) {
     *   // Wait for user to decide what to do
     *   const bool user_wants_reset { true };
     *   if (user_wants_reset) {
     *     session_t::get().reset_persistence();
     *   }
     * }
     * ```
     */
    void
    reset_persistence(const topology::mutation_capability_t &capability, automatic_vdd_policy_e vdd_policy);

    /**
     * @brief Create VDD monitor
     * @param client_name 客户端名称，用于驱动识别客户端并启动对应的显示器
     */
    bool
    create_vdd_monitor(const topology::mutation_capability_t &capability, const std::string &client_name = "");

    /**
     * Create a VDD without displaying Core UI. Intended for tray providers.
     */
    bool
    create_vdd_monitor_noninteractive(const topology::mutation_capability_t &capability);

    /**
     * @brief Destroy VDD monitor
     */
    bool
    destroy_vdd_monitor(const topology::mutation_capability_t &capability);

    /**
     * @brief Check if display is on
     */
    bool
    is_display_on();

    /**
     * @brief A deleted copy constructor for singleton pattern.
     * @note Public to ensure better error message.
     */
    session_t(session_t const &) = delete;

    /**
     * @brief A deleted assignment operator for singleton pattern.
     * @note Public to ensure better error message.
     */
    void
    operator=(session_t const &) = delete;

  private:
#ifdef SUNSHINE_TEST_DISPLAY_SESSION
    friend struct session_test_access_t;
#endif
    /**
     * @brief A class for retrying to set/reset state.
     *
     * This timer class spins a thread which is mostly sleeping all the time, but can be
     * configured to wake up every X seconds.
     *
     * The timer posts generation-checked commands; only the topology worker touches
     * settings under the session mutex. Listener notifications carry request identity,
     * never a capability or an untracked mutation callback.
     */
    class StateRetryTimer;

    enum class vdd_mode_update_e {
      ready,
      recreate_monitor,
      failed,
    };

    enum class vdd_stage_result_e {
      ready,
      create_failed,
      /** Creation failed and the driver's control channel is dead: only an adapter restart can recover it. */
      adapter_restart_required,
      topology_failed,
      modes_failed,
    };

    struct pending_vdd_context_t {
      boost::optional<active_topology_t> initial_topology;
      boost::optional<device_info_map_t> pre_vdd_devices;

      void
      reset() {
        initial_topology.reset();
        pre_vdd_devices.reset();
      }
    };

    /**
     * @brief A private constructor to ensure the singleton pattern.
     * @note Cannot be defaulted in declaration because of forward declared StateRetryTimer.
     */
    explicit session_t();

    /**
     * @brief An implementation of `restore_state` without a mutex lock.
     * @param reason The reason for reverting settings, used to determine appropriate cleanup behavior.
     * @see restore_state for the description.
     */
    void
    restore_state_impl(const topology::mutation_capability_t &capability, automatic_vdd_policy_e vdd_policy,
      revert_reason_e reason = revert_reason_e::stream_ended);

    /**
     * @brief Undo a failed VDD preparation. When the adapter must be restarted,
     * the restart runs as a supervised actor and the restoration is deferred to
     * the coordinator until it settles; otherwise restore now. The mutex must be held.
     */
    void
    restore_after_failed_vdd(const topology::mutation_capability_t &capability, automatic_vdd_policy_e vdd_policy,
      vdd_stage_result_e result);

    /**
     * @brief Start polling mechanism as fallback when CCD API is temporarily unavailable.
     * @param reason The reason for reverting settings.
     */
    void
    start_polling_restore(revert_reason_e reason);

    void restore_settings_or_defer(const topology::mutation_capability_t &capability, revert_reason_e reason);
    void schedule_startup_restore_retry();

    /**
     * @brief 执行依赖可访问交互式 Windows 会话的 VDD 显示操作。
     */
    static vdd_stage_result_e
    apply_vdd_display_stage(const topology::mutation_capability_t &capability, const parsed_config_t &config,
      const boost::optional<device_info_map_t> &pre_vdd_devices);

    settings_t settings; /**< A class for managing display device settings. */
    std::mutex mutex; /**< A mutex for ensuring thread-safety. */
    std::string current_vdd_client_id; /**< Current client ID associated with VDD monitor. */
    std::optional<vdd_utils::hdr_brightness_t> current_vdd_hdr_brightness; /**< HDR capabilities currently programmed into VDD. */
    std::string original_output_name; /**< Original output_name value before VDD device ID was set. */
    boost::optional<parsed_config_t::device_prep_e> current_device_prep; /**< Current device preparation mode, respecting client overrides. */
    boost::optional<parsed_config_t::vdd_prep_e> current_vdd_prep; /**< Current VDD preparation mode for VDD mode sessions. */
    boost::optional<bool> current_use_vdd; /**< Whether current session is using VDD mode. */
    pending_vdd_context_t pending_vdd_; /**< 在显示配置成功或清理前保留的 VDD 创建基线。 */
    std::atomic_bool pending_restore_ { false }; /**< Read-only admission probe never takes the session mutex. */
    boost::atomic<int> polling_retry_count_ {0}; /**< Retry counter for polling restore mechanism. */
    std::atomic_bool awaiting_unlock_ { false }; /**< A deferred restore neither polls nor counts until unlock, as in base 204a968. */

    /**
     * @brief An instance of StateRetryTimer.
     * @warning MUST BE declared after the settings and mutex members to ensure proper destruction order!.
     */
    std::unique_ptr<StateRetryTimer> timer;

    /**
     * @brief 准备 VDD 驱动和显示器，不修改 Windows 显示拓扑。
     * @param config 已解析的会话配置；成功时写回 VDD 设备 ID。
     * @param session 用于标识和配置显示器的启动会话。
     * @param pre_vdd_devices 接收新建显示器前捕获的物理显示器快照；
     *        有值但为空表示主机确实无头。
     */
    vdd_stage_result_e
    prepare_vdd(const topology::mutation_capability_t &capability, parsed_config_t &config,
      const rtsp_stream::launch_session_t &session,
      boost::optional<device_info_map_t> &pre_vdd_devices);

    vdd_mode_update_e
    update_vdd_resolution(const topology::mutation_capability_t &capability, const parsed_config_t &config,
      const vdd_utils::VddSettings &vdd_settings,
      const std::string &vdd_device_id);

    /**
     * @brief Clear VDD session state.
     * @note This method does NOT acquire the mutex! It is intended to be used from places
     *       where the mutex has already been locked.
     */
    void
    clear_vdd_state();

    /**
     * @brief 取消旧的延迟配置或恢复任务，并清除对应的待处理状态。
     * @note 调用方必须已经持有 mutex。
     */
    void
    cancel_pending_display_retry();

    /**
     * @brief Stop timer and clear VDD state
     * @note This method does NOT acquire the mutex! It is intended to be used from places
     *       where the mutex has already been locked.
     */
    void
    stop_timer_and_clear_vdd_state();
  };

}  // namespace display_device
