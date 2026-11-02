/**
 * @file src/video.h
 * @brief Declarations for video.
 */
#pragma once

#include "input.h"
#include "platform/common.h"
#include "thread_safe.h"
#include "video_admission.h"
#include "video_colorspace.h"
#include "video_encoder_capacity.h"

#include <boost/smart_ptr/shared_ptr.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "hdr/client_display_capabilities.h"
#include "streaming/bitrate.h"
#include "streaming/capture_target.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

struct AVPacket;
namespace image_enhancement {
  struct backend_use_t;
}

namespace video {

  /**
   * Runtime state for one active Windows frame-conversion pipeline.
   *
   * The Web UI uses this to report what Sunshine actually initialized, rather
   * than inferring state from saved configuration alone.
   */
  struct hdr_pipeline_status_t {
    std::uint64_t id {};
    std::string hdr_mode { "sdr" };
    std::string dv_profile;
    std::string dv_state { "off" };
    std::string analysis_mode { "off" };
    bool analysis_active {};
    bool scene_metadata_active {};
    std::vector<std::string> metadata_formats;
    std::string conversion_path { "pixel_shader" };
    std::string conversion_fallback_reason;
    std::string analysis_failure_reason;
    std::string synthetic_hdr_backend { "none" };
    std::string synthetic_hdr_state { "disabled" };
    std::string synthetic_hdr_failure_reason;
    std::string nr_backend { "none" };
    std::string nr_state { "disabled" };
    std::string nr_failure_reason;
    bool nr_toggle_supported {};
    bool nr_requested_enabled {};
    int nr_requested_scale_percent = 100;
    int nr_scale_percent = 100;
    float nr_requested_intensity = 1.0f, nr_intensity = 1.0f;
    bool nr_requested_ui_correction = false, nr_ui_correction = false;
    int nr_requested_motion_quality = 0, nr_motion_quality = 0;
    int nr_requested_style = 0, nr_style = 0;
    float nr_requested_skin_structure_strength = 0.0f, nr_skin_structure_strength = 0.0f;
    bool nr_requested_auto_mask = false, nr_auto_mask = false;
    std::uint64_t nr_request_revision = 0;
    std::uint32_t nr_source_width {}, nr_source_height {};
    std::string nr_settings_failure_reason;
  };

  std::uint64_t
  register_hdr_pipeline_status(const hdr_pipeline_status_t &status);

  void
  update_hdr_pipeline_status(std::uint64_t id, const hdr_pipeline_status_t &status);

  void
  unregister_hdr_pipeline_status(std::uint64_t id);

  std::vector<hdr_pipeline_status_t>
  get_hdr_pipeline_statuses();

  // Requests are consumed only by the owning conversion thread at a frame boundary.
  struct nr_request_t {
    bool enabled;
    int scale_percent;
    float intensity = 1.0f;
    bool ui_correction = false;
    int motion_quality = 0;
    std::uint64_t revision = 0;
    int style = 0;
    float skin_structure_strength = 0.0f;
    bool auto_mask = false;
  };
  int request_nr_enabled(std::uint64_t id, bool enabled, std::optional<int> scale_percent = std::nullopt,
    std::optional<float> intensity = std::nullopt, std::optional<bool> ui_correction = std::nullopt,
    std::optional<int> motion_quality = std::nullopt,
    std::optional<int> style = std::nullopt, std::optional<float> skin_structure_strength = std::nullopt,
    std::optional<bool> auto_mask = std::nullopt);
  std::optional<nr_request_t> requested_nr_settings(std::uint64_t id);
  bool rollback_nr_settings(std::uint64_t id, const nr_request_t &failed, const nr_request_t &previous);

  // 动态参数调节类型
  enum class dynamic_param_type_e : int {
    RESOLUTION,        // 分辨率 - 值：2个int (width, height)
    FPS,               // 帧率 - 值：1个float
    BITRATE,           // 码率 (Kbps) - 值：1个int
    QP,                // 量化参数 - 值：1个int
    FEC_PERCENTAGE,    // FEC百分比 - 值：1个int
    PRESET,            // 编码预设 - 值：1个int
    ADAPTIVE_QUANTIZATION, // 自适应量化 - 值：1个bool
    MULTI_PASS,        // 多遍编码 - 值：1个int
    VBV_BUFFER_SIZE,   // VBV缓冲区大小 - 值：1个int
    // Wire value 9; keep this explicit because the enum ordinal is part of the
    // Sunshine dynamic-parameter control protocol.
    CLIENT_SDR_WHITE_NITS = 9, // Reserved legacy wire ID; ignored, never reuse
    MAX_PARAM_TYPE
  };

  static_assert(static_cast<int>(dynamic_param_type_e::CLIENT_SDR_WHITE_NITS) == 9);

  // 动态参数值联合体
  union dynamic_param_value_t {
    int int_value;
    int int_array_value[2];
    bool bool_value;
    float float_value;
  };

  // 动态参数结构
  struct dynamic_param_t {
    dynamic_param_type_e type;
    dynamic_param_value_t value;
    bool valid;
  };

  // 动态参数调节事件类型
  using dynamic_param_change_event_t = safe::mail_raw_t::event_t<dynamic_param_t>;

  /**
   * A launch's admission, carried from the probe that granted it to the
   * capture attach that commits it. It is valid only against the encoder
   * selection it was granted on, and commits at most once. While the session
   * captures, `lease` holds its committed admission; a replacement mode swaps
   * it (make-before-break) until the capture ends and closes the ticket.
   */
  struct admission_ticket_t {
    std::mutex lock;
    admission::pending_t pending;
    std::uint64_t encoder_generation {};
    admission::lease_t lease;
    /** The capture committed `pending` and holds `lease`. */
    bool attached {};
    /** The capture ended: nothing is held, and no replacement may be committed. */
    bool closed {};
  };

  /**
   * Reserve a session's replacement mode while its current admission stays held
   * (make-before-break): same adapter and frame rate, new size. Refused unless
   * the session is capturing. Unaccounted when its admission was.
   */
  std::expected<admission::pending_t, std::string>
  reserve_replacement(const std::shared_ptr<admission_ticket_t> &ticket, std::uint32_t width, std::uint32_t height);

  /**
   * Commit a replacement from `reserve_replacement` and release the admission it
   * replaces. Refused (the reservation rolls back, the old lease stays) when the
   * capture ended or the encoder selection moved since the ticket was granted.
   */
  std::expected<void, std::string>
  commit_replacement(const std::shared_ptr<admission_ticket_t> &ticket, admission::pending_t replacement);

  struct exact_move_state_t;

  /**
   * Fences an exact capture through a managed head mode change that may re-create
   * the head under a new display id. It keeps the live capture of that display
   * running while held, and that capture waits for a missing display instead of
   * ending; `retarget` moves that capture, and its source, to the head's new
   * display. Released without a retarget, a display that is gone ends the
   * capture as before.
   */
  class exact_capture_move_t {
  public:
    explicit exact_capture_move_t(std::string display);
    ~exact_capture_move_t();
    exact_capture_move_t(const exact_capture_move_t &) = delete;
    exact_capture_move_t &
    operator=(const exact_capture_move_t &) = delete;

    /** Refused unless the capture held at construction still captures the current display, or when another capture owns @p display. */
    std::expected<void, std::string>
    retarget(const std::string &display);

  private:
    std::string _display;
    std::shared_ptr<exact_move_state_t> _state;
    std::shared_ptr<void> _source;  ///< A ref of the held capture.
  };

  /**
   * Whether the encoder follows a host display size change. A managed (exact)
   * head only changes size on the client's request, so it always follows; other
   * displays honour `dynamic_resolution_follow_display`.
   */
  constexpr bool
  follows_display_resolution(bool display_exact, bool follow_setting) {
    return display_exact || follow_setting;
  }

  /* Encoding configuration requested by remote client */
  struct config_t {
    int width;  // Video width in pixels
    int height;  // Video height in pixels
    int framerate;  // Requested framerate, used in individual frame bitrate budget calculation
    int bitrate;  // Video bitrate in kilobits (1000 bits) for requested framerate
    int slicesPerFrame;  // Number of slices per frame
    int numRefFrames;  // Max number of reference frames

    /* Requested color range and SDR encoding colorspace, HDR encoding colorspace is always BT.2020+ST2084
       Color range (encoderCscMode & 0x1) : 0 - limited, 1 - full
       SDR encoding colorspace (encoderCscMode >> 1) : 0 - BT.601, 1 - BT.709, 2 - BT.2020 */
    int encoderCscMode;

    int videoFormat;  // 0 - H.264, 1 - HEVC, 2 - AV1

    /* Encoding color depth and HDR transfer function:
       0 - SDR 8-bit
       1 - HDR 10-bit with PQ (SMPTE ST 2084)
       2 - HDR 10-bit with HLG (ARIB STD-B67)
       HDR encoding activates when dynamicRange > 0 and the display is operating in HDR mode */
    int dynamicRange;

    /* Selected dynamic HDR format for this session, as hdr::dynamic_hdr_format_e
       (0 none, 1 HDR10+, 2 vivid PQ, 3 vivid HLG, 4 Dolby Vision Profile 8.1).
       Negotiated once in the RTSP ANNOUNCE from the client's reported capabilities;
       the encode path gates Dolby Vision RPU injection on it. */
    int dynamic_hdr_format = 0;

    int chromaSamplingType;  // 0 - 4:2:0, 1 - 4:4:4

    int enableIntraRefresh;  // 0 - disabled, 1 - enabled

    // NTSC framerate support: use frameRateNum/frameRateDen for precise framerate
    // e.g., 120000/1001 = 119.88fps (NTSC), 60000/1001 = 59.94fps
    // When frameRateDen is 0 or 1, use integer framerate
    int frameRateNum = 0;  // Framerate numerator (0 = use integer framerate)
    int frameRateDen = 1;  // Framerate denominator

    // Display name for screen capture (specified by client)
    // If empty, use the default display from global configuration
    std::string display_name;

    // Managed topology: capture exactly display_name or nothing (no default-display fallback).
    bool display_exact = false;
    // Optional per-display initialization capture backend override.
    // This is intentionally scoped to a single platf::display() call so encoder
    // probing can avoid mutating the global config::video.capture string.
    std::string capture_backend_override;

    // Remote display target. This adjusts only metadata sent to the client;
    // it must never mutate a physical host display's EDID, HDR state, or ICC.
    hdr::client_display_capabilities_t hdr_capabilities;

    // Orthogonal session contracts. Keep these defaulted fields after the
    // legacy aggregate-initialized fields above. During migration, callers
    // that have not resolved them explicitly retain legacy behavior through
    // effective_frame_pipeline_policy(). Capture backends must consume the
    // effective capture contract instead of reading dynamicRange directly.
    platf::frame_pipeline_policy_t frame_pipeline_policy;
    bool frame_pipeline_policy_resolved = false;
    platf::pre_encode_filter_e pre_encode_filter = platf::pre_encode_filter_e::none;
    platf::pre_encode_filter_config_t pre_encode_filter_config;
    boost::shared_ptr<const image_enhancement::backend_use_t> enhancement_backend;
    // Local diagnostics only; never serialized into the media protocol.
    std::uint32_t perf_session_id = 0;
    bool pyrowave_dynamic_hdr_mapping = false;
    std::shared_ptr<streaming::bitrate_budget_t> bitrate_budget;
    std::shared_ptr<streaming::capture_target_t> capture_target;
    // The launch's admission, committed once before the capture source is
    // acquired; its lease lives as long as the session's capture ref.
    std::shared_ptr<admission_ticket_t> admission;
    // Probe only, managed profile only: the encoder records its driver-reported
    // capacity for the admission budget. False keeps every driver call unchanged.
    bool report_admission_capacity = false;

    platf::frame_pipeline_policy_t
    effective_frame_pipeline_policy() const {
      if (frame_pipeline_policy_resolved) {
        return frame_pipeline_policy;
      }
      return platf::resolve_frame_pipeline_policy(dynamicRange, false);
    }

    // Helper to get effective framerate as double
    double get_effective_framerate() const {
      if (frameRateNum > 0 && frameRateDen > 0) {
        return static_cast<double>(frameRateNum) / frameRateDen;
      }
      return static_cast<double>(framerate);
    }
  };

  // Resolve source mastering metadata after optional HDR pixel processing.
  bool
  get_effective_hdr_metadata(platf::display_t *display, const config_t &config, SS_HDR_METADATA &metadata);

  // Convert a total video transport budget (including FEC) to encoder bitrate.
  int
  encoder_bitrate_from_total_bitrate(int total_bitrate_kbps, int fec_percentage);

  // Cap a dynamic total-bitrate request, then convert it to encoder bitrate.
  int
  encoder_bitrate_for_total_request(int requested_total_bitrate_kbps, int max_total_bitrate_kbps, int fec_percentage);

  // Cap an initial bitrate that has already been converted to an encoder budget.
  int
  cap_initial_encoder_bitrate(int initial_encoder_bitrate_kbps, int max_total_bitrate_kbps, int fec_percentage);

  struct input_activity_boost_policy_t {
    bool configured {};
    bool useful {};
    int fps {};
    std::chrono::duration<double, std::milli> frame_time {};
  };

  struct input_activity_boost_config_t {
    bool variable_refresh_rate {};
    bool enabled {};
    int stream_fps {};
    int minimum_fps_target {};
    int boost_fps {};
    int window_ms {};
  };

  std::chrono::duration<double, std::milli>
  minimum_frame_time_for_vrr(int stream_fps, int minimum_fps_target);

  input_activity_boost_policy_t
  make_input_activity_boost_policy(const input_activity_boost_config_t &config);

  std::chrono::duration<double, std::milli>
  effective_minimum_frame_time(
    const std::chrono::duration<double, std::milli> &base_minimum_frame_time,
    const input_activity_boost_policy_t &input_activity_boost_policy,
    bool input_boost_active,
    int minimum_fps_target);

  platf::mem_type_e
  map_base_dev_type(AVHWDeviceType type);
  platf::pix_fmt_e
  map_pix_fmt(AVPixelFormat fmt);

  void
  free_ctx(AVCodecContext *ctx);
  void
  free_frame(AVFrame *frame);
  void
  free_buffer(AVBufferRef *ref);

  using avcodec_ctx_t = util::safe_ptr<AVCodecContext, free_ctx>;
  using avcodec_frame_t = util::safe_ptr<AVFrame, free_frame>;
  using avcodec_buffer_t = util::safe_ptr<AVBufferRef, free_buffer>;
  using sws_t = util::safe_ptr<SwsContext, sws_freeContext>;
  struct encoder_platform_formats_t {
    virtual ~encoder_platform_formats_t() = default;
    platf::mem_type_e dev_type;
    platf::pix_fmt_e pix_fmt_8bit, pix_fmt_10bit;
    platf::pix_fmt_e pix_fmt_yuv444_8bit, pix_fmt_yuv444_10bit;
  };

  struct encoder_platform_formats_avcodec: encoder_platform_formats_t {
    using init_buffer_function_t = std::function<util::Either<avcodec_buffer_t, int>(platf::avcodec_encode_device_t *)>;

    encoder_platform_formats_avcodec(
      const AVHWDeviceType &avcodec_base_dev_type,
      const AVHWDeviceType &avcodec_derived_dev_type,
      const AVPixelFormat &avcodec_dev_pix_fmt,
      const AVPixelFormat &avcodec_pix_fmt_8bit,
      const AVPixelFormat &avcodec_pix_fmt_10bit,
      const AVPixelFormat &avcodec_pix_fmt_yuv444_8bit,
      const AVPixelFormat &avcodec_pix_fmt_yuv444_10bit,
      const init_buffer_function_t &init_avcodec_hardware_input_buffer_function):
        avcodec_base_dev_type { avcodec_base_dev_type },
        avcodec_derived_dev_type { avcodec_derived_dev_type },
        avcodec_dev_pix_fmt { avcodec_dev_pix_fmt },
        avcodec_pix_fmt_8bit { avcodec_pix_fmt_8bit },
        avcodec_pix_fmt_10bit { avcodec_pix_fmt_10bit },
        avcodec_pix_fmt_yuv444_8bit { avcodec_pix_fmt_yuv444_8bit },
        avcodec_pix_fmt_yuv444_10bit { avcodec_pix_fmt_yuv444_10bit },
        init_avcodec_hardware_input_buffer { init_avcodec_hardware_input_buffer_function } {
      dev_type = map_base_dev_type(avcodec_base_dev_type);
      pix_fmt_8bit = map_pix_fmt(avcodec_pix_fmt_8bit);
      pix_fmt_10bit = map_pix_fmt(avcodec_pix_fmt_10bit);
      pix_fmt_yuv444_8bit = map_pix_fmt(avcodec_pix_fmt_yuv444_8bit);
      pix_fmt_yuv444_10bit = map_pix_fmt(avcodec_pix_fmt_yuv444_10bit);
    }

    AVHWDeviceType avcodec_base_dev_type, avcodec_derived_dev_type;
    AVPixelFormat avcodec_dev_pix_fmt;
    AVPixelFormat avcodec_pix_fmt_8bit, avcodec_pix_fmt_10bit;
    AVPixelFormat avcodec_pix_fmt_yuv444_8bit, avcodec_pix_fmt_yuv444_10bit;

    init_buffer_function_t init_avcodec_hardware_input_buffer;
  };

  struct encoder_platform_formats_nvenc: encoder_platform_formats_t {
    encoder_platform_formats_nvenc(
      const platf::mem_type_e &dev_type,
      const platf::pix_fmt_e &pix_fmt_8bit,
      const platf::pix_fmt_e &pix_fmt_10bit,
      const platf::pix_fmt_e &pix_fmt_yuv444_8bit,
      const platf::pix_fmt_e &pix_fmt_yuv444_10bit) {
      encoder_platform_formats_t::dev_type = dev_type;
      encoder_platform_formats_t::pix_fmt_8bit = pix_fmt_8bit;
      encoder_platform_formats_t::pix_fmt_10bit = pix_fmt_10bit;
      encoder_platform_formats_t::pix_fmt_yuv444_8bit = pix_fmt_yuv444_8bit;
      encoder_platform_formats_t::pix_fmt_yuv444_10bit = pix_fmt_yuv444_10bit;
    }
  };

  struct encoder_platform_formats_amf: encoder_platform_formats_t {
    encoder_platform_formats_amf(
      const platf::mem_type_e &dev_type,
      const platf::pix_fmt_e &pix_fmt_8bit,
      const platf::pix_fmt_e &pix_fmt_10bit,
      const platf::pix_fmt_e &pix_fmt_yuv444_8bit,
      const platf::pix_fmt_e &pix_fmt_yuv444_10bit) {
      encoder_platform_formats_t::dev_type = dev_type;
      encoder_platform_formats_t::pix_fmt_8bit = pix_fmt_8bit;
      encoder_platform_formats_t::pix_fmt_10bit = pix_fmt_10bit;
      encoder_platform_formats_t::pix_fmt_yuv444_8bit = pix_fmt_yuv444_8bit;
      encoder_platform_formats_t::pix_fmt_yuv444_10bit = pix_fmt_yuv444_10bit;
    }
  };

  struct encoder_t {
    std::string_view name;
    enum flag_e {
      PASSED,  ///< Indicates the encoder is supported.
      REF_FRAMES_RESTRICT,  ///< Set maximum reference frames.
      DYNAMIC_RANGE,  ///< HDR support.
      YUV444,  ///< YUV 4:4:4 support.
      VUI_PARAMETERS,  ///< AMD encoder with VAAPI doesn't add VUI parameters to SPS.
      MAX_FLAGS  ///< Maximum number of flags.
    };

    static std::string_view
    from_flag(flag_e flag) {
#define _CONVERT(x) \
  case flag_e::x:   \
    return std::string_view(#x)
      switch (flag) {
        _CONVERT(PASSED);
        _CONVERT(REF_FRAMES_RESTRICT);
        _CONVERT(DYNAMIC_RANGE);
        _CONVERT(YUV444);
        _CONVERT(VUI_PARAMETERS);
        _CONVERT(MAX_FLAGS);
      }
#undef _CONVERT

      return { "unknown" };
    }

    struct option_t {
      KITTY_DEFAULT_CONSTR_MOVE(option_t)
      option_t(const option_t &) = default;

      std::string name;
      std::variant<int, int *, std::optional<int> *, std::function<int()>, std::string, std::string *, std::function<const std::string(const config_t &)>> value;

      option_t(std::string &&name, decltype(value) &&value):
          name { std::move(name) }, value { std::move(value) } {}
    };

    const std::unique_ptr<const encoder_platform_formats_t> platform_formats;

    struct codec_t {
      std::vector<option_t> common_options;
      std::vector<option_t> sdr_options;
      std::vector<option_t> hdr_options;
      std::vector<option_t> sdr444_options;
      std::vector<option_t> hdr444_options;
      std::vector<option_t> fallback_options;

      std::string name;
      std::bitset<MAX_FLAGS> capabilities;

      bool
      operator[](flag_e flag) const {
        return capabilities[(std::size_t) flag];
      }

      std::bitset<MAX_FLAGS>::reference
      operator[](flag_e flag) {
        return capabilities[(std::size_t) flag];
      }
    } av1, hevc, h264;

    const codec_t &
    codec_from_config(const config_t &config) const {
      switch (config.videoFormat) {
        default:
          BOOST_LOG(error) << "Unknown video format " << config.videoFormat << ", falling back to H.264";
          // fallthrough
        case 0:
          return h264;
        case 1:
          return hevc;
        case 2:
          return av1;
      }
    }

    uint32_t flags;
  };

  struct encode_session_t {
    virtual ~encode_session_t() = default;

    std::uint32_t perf_session_id = 0;

    virtual int
    convert(platf::img_t &img) = 0;

    virtual void
    request_idr_frame() = 0;

    virtual void
    request_normal_frame() = 0;

    virtual void
    invalidate_ref_frames(int64_t first_frame, int64_t last_frame) = 0;

    // Accepts an encoder target; transport/FEC allocation happens once upstream.
    virtual bool
    set_bitrate(int bitrate_kbps) = 0;

    std::shared_ptr<streaming::bitrate_budget_t> bitrate_budget;

    virtual void
    set_dynamic_param(const dynamic_param_t &param) = 0;  // 新增：通用动态参数调整方法

    unsigned
    fec_percentage() const {
      return bitrate_budget ? bitrate_budget->applied().fec_percentage : 0;
    }

    /** What the encoder reported when created with `report_admission_capacity`; empty otherwise. */
    virtual encoder_capacity_t
    admission_capacity() const {
      return {};
    }
  };

  // encoders
  extern encoder_t software;

#if !defined(__APPLE__)
  extern encoder_t nvenc;  // available for windows and linux
#endif

#ifdef _WIN32
  extern encoder_t amdvce;
  extern encoder_t quicksync;
#endif

#ifdef __linux__
  extern encoder_t vaapi;
#endif

#ifdef __APPLE__
  extern encoder_t videotoolbox;
#endif

  struct packet_raw_t {
    virtual ~packet_raw_t() = default;

    virtual bool
    is_idr() = 0;

    virtual int64_t
    frame_index() = 0;

    virtual uint8_t *
    data() = 0;

    virtual size_t
    data_size() = 0;

    struct replace_t {
      std::string_view old;
      std::string_view _new;

      KITTY_DEFAULT_CONSTR_MOVE(replace_t)

      replace_t(std::string_view old, std::string_view _new) noexcept:
          old { std::move(old) }, _new { std::move(_new) } {}
    };

    std::vector<replace_t> *replacements = nullptr;
    void *channel_data = nullptr;
    bool after_ref_frame_invalidation = false;
    unsigned fec_percentage = 0;  // Protection budget when this frame was submitted.
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
    std::optional<platf::frame_pipeline_trace_t> pipeline_trace;
  };

  struct packet_raw_avcodec: packet_raw_t {
    packet_raw_avcodec() {
      av_packet = av_packet_alloc();
    }

    ~packet_raw_avcodec() {
      av_packet_free(&this->av_packet);
    }

    bool
    is_idr() override {
      return av_packet->flags & AV_PKT_FLAG_KEY;
    }

    int64_t
    frame_index() override {
      return av_packet->pts;
    }

    uint8_t *
    data() override {
      return av_packet->data;
    }

    size_t
    data_size() override {
      return av_packet->size;
    }

    AVPacket *av_packet;
  };

  struct packet_raw_generic: packet_raw_t {
    packet_raw_generic(std::vector<uint8_t> &&frame_data, int64_t frame_index, bool idr):
        frame_data { std::move(frame_data) }, index { frame_index }, idr { idr } {
    }

    bool
    is_idr() override {
      return idr;
    }

    int64_t
    frame_index() override {
      return index;
    }

    uint8_t *
    data() override {
      return frame_data.data();
    }

    size_t
    data_size() override {
      return frame_data.size();
    }

    std::vector<uint8_t> frame_data;
    int64_t index;
    bool idr;
  };

  using packet_t = std::unique_ptr<packet_raw_t>;

  struct hdr_info_raw_t {
    explicit hdr_info_raw_t(bool enabled):
        enabled { enabled }, metadata {} {};
    explicit hdr_info_raw_t(bool enabled, const SS_HDR_METADATA &metadata):
        enabled { enabled }, metadata { metadata } {};

    bool enabled;
    SS_HDR_METADATA metadata;
  };

  using hdr_info_t = std::unique_ptr<hdr_info_raw_t>;

  extern int active_hevc_mode;
  extern int active_av1_mode;
  extern bool last_encoder_probe_supported_ref_frames_invalidation;
  extern std::array<bool, 3> last_encoder_probe_supported_yuv444_for_codec;  // 0 - H.264, 1 - HEVC, 2 - AV1

  enum class probe_error_e {
    none,
    no_active_display,
    configured_encoder_unavailable,
    codec_requirements_unmet,
    no_working_encoder
  };

  struct probe_result_t {
    probe_error_e error;
    std::string message;
    std::string hint;

    explicit operator bool() const {
      return error == probe_error_e::none;
    }
  };

  extern probe_result_t last_encoder_probe_result;

  enum class probe_target_policy_e {
    backend_autoselect,
    exact,
    vdd_compatible
  };

  /** The mode a live probe is admitted for. */
  struct probe_mode_t {
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t fps {};
  };

  struct probe_target_t {
    std::string output_name; /**< Device selector in the same domain as config::video.output_name. */
    probe_target_policy_e policy { probe_target_policy_e::backend_autoselect };
    /** Required by an enforced admission budget; unused otherwise. */
    std::optional<probe_mode_t> mode {};
  };

  /** One probe call's own result; never shared with another call. */
  struct probe_outcome_t {
    int status { -1 };
    probe_result_t result { probe_error_e::no_working_encoder, {}, {} };
    /** The encoder selection this outcome was decided against. */
    std::uint64_t encoder_generation {};
    /** Live probes only: the admission to commit before the source is attached. */
    admission::pending_t admission {};
    /** The admission budget refused this, not the encoder or the display: no failure cooldown. */
    bool budget_refusal {};
  };

  /**
   * @brief Return the encoder selected by the latest successful probe.
   * @return Encoder identifier, or an empty string when no encoder is active.
   */
  std::string
  active_encoder_name();

  void
  capture(
    safe::mail_t mail,
    config_t config,
    void *channel_data,
    std::optional<safe::mail_raw_t::event_t<dynamic_param_t>> dynamic_param_events = std::nullopt,
    int packet_size = 0);

  bool
  validate_encoder(
    encoder_t &encoder,
    bool expect_failure,
    const std::optional<std::string> &probe_capture_override,
    const std::string &probe_display_name);

  /**
   * @brief Probe encoders and select the preferred encoder.
   * This is called once at startup and each time a stream is launched to
   * ensure the best encoder is selected. Encoder availability can change
   * at runtime due to all sorts of things from driver updates to eGPUs.
   *
   * @warning This is only safe to call when there is no client actively streaming.
   */
  int
  probe_encoders(std::optional<probe_target_t> target = std::nullopt);

  /**
   * @brief Probe like probe_encoders, returning this call's own outcome.
   * A live probe (a source is already capturing) does not write
   * last_encoder_probe_result and returns its admission; a global probe also
   * publishes last_encoder_probe_result as probe_encoders does.
   */
  probe_outcome_t
  probe_encoders_with_outcome(std::optional<probe_target_t> target = std::nullopt);

  /**
   * @brief Admit an exact target against the current encoder selection without
   * reprobing: a joiner of an already prepared topology. Never changes the
   * chosen encoder or last_encoder_probe_result, so a ticket granted to the
   * launch it joins stays valid.
   */
  probe_outcome_t
  admit_existing(const probe_target_t &target);

  /**
   * Install the production admission budget for a managed profile, once; later
   * calls keep the installed budget. The blank profile never calls this, so it
   * keeps no budget and admits unaccounted, as before.
   */
  void
  install_managed_admission_budget();

  namespace testing {
    /** Install the live admission budget; null disables accounting (the production default). */
    void
    set_admission_budget(std::shared_ptr<admission::budget_t> budget);

    /** Whether a probe now asks encoders for their capacity: only under an enforced budget. */
    bool
    capacity_reporting_requested();

    /** What the end of a successful encoder validation does with the probe's reported capacity. */
    void
    learn_validated_capacity(std::uint64_t adapter_luid, const encoder_capacity_t &capacity);

    /** Advances on every change of the chosen encoder. */
    std::uint64_t
    encoder_generation();

    /**
     * Replace the global probe's encoder validation (empty restores it). The
     * replacement returns the probe status; 0 keeps the chosen encoder.
     */
    void
    set_global_probe(std::function<int()> probe);

    using capture_display_factory_t = std::function<std::shared_ptr<platf::display_t>(platf::mem_type_e, const std::string &, const config_t &)>;
    using capture_display_names_t = std::function<std::vector<std::string>(platf::mem_type_e)>;
    /** The adapter LUID an output is attached to; nullopt when unknown. */
    using capture_display_adapter_t = std::function<std::optional<std::uint64_t>(const std::string &)>;

    /**
     * Replace the platform display edge of the capture path (platf::display,
     * platf::display_names and the output adapter lookup). Empty functions
     * restore the platform. Also selects the software encoder's device type
     * when no encoder has been probed.
     */
    void
    set_capture_platform(capture_display_factory_t display, capture_display_names_t names, capture_display_adapter_t adapter = {});

    /** A capture session attached exactly as capture_async attaches, without an encoder. */
    class capture_attachment_t {
    public:
      virtual ~capture_attachment_t() = default;
      /** The next captured image, or null on timeout or when the source stopped. */
      virtual std::shared_ptr<platf::img_t>
      pop(std::chrono::milliseconds timeout) = 0;
    };

    /** Null when the capture source refuses the session. */
    std::unique_ptr<capture_attachment_t>
    attach_capture(const config_t &config);

    /** The process-global encoder streams start with; empty when none. */
    std::string
    chosen_encoder_name();

    /**
     * Makes the first non-software encoder the chosen one for its lifetime, as
     * if it had been validated on the given adapter (0: no adapter recorded).
     */
    class scoped_hardware_encoder_t {
    public:
      explicit scoped_hardware_encoder_t(std::uint64_t validated_adapter_luid);
      ~scoped_hardware_encoder_t();
      scoped_hardware_encoder_t(const scoped_hardware_encoder_t &) = delete;
      scoped_hardware_encoder_t &operator=(const scoped_hardware_encoder_t &) = delete;

    private:
      void *_previous;
      void *_selected = nullptr;
      std::optional<std::uint64_t> _previous_luid;
    };
  }  // namespace testing
}  // namespace video
