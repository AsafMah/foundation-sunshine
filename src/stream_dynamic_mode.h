/**
 * @file src/stream_dynamic_mode.h
 * @brief Gate for client-driven resolution/FPS change requests (provisional pending G1 hardware).
 *
 * A managed native head is admitted, prepared and captured at one exact mode. The
 * display topology owner denies `reconfigure` for managed heads and the encoder
 * follows the captured display size, so applying a client mode request there would
 * only make the session config and perf report diverge from what is streamed.
 * Managed sessions therefore refuse such requests explicitly; legacy sessions run
 * the unchanged legacy handler.
 */
#pragma once

#include <sstream>
#include <string>
#include <utility>

namespace stream::dynamic_mode {
  enum class kind_e {
    resolution,
    fps,
  };

  struct request_t {
    kind_e kind;
    int width = 0;
    int height = 0;
    float rate = 0.0f;

    static request_t
    resolution(int width, int height) {
      return { kind_e::resolution, width, height, 0.0f };
    }

    static request_t
    fps(float value) {
      return { kind_e::fps, 0, 0, value };
    }
  };

  enum class decision_e {
    applied,  ///< Legacy session: the legacy handler ran.
    unchanged,  ///< Managed session asked for its current mode; nothing to do.
    refused_managed,  ///< Managed session: refused without mutating anything.
  };

  struct outcome_t {
    decision_e decision;
    std::string message;  ///< INFO line for a refusal; empty otherwise.
  };

  /**
   * Run @p apply_legacy only for a legacy (non-managed) session. A managed session
   * never runs it: a request for a different mode is refused with an INFO message.
   */
  template <class Apply>
  outcome_t
  gate(bool managed, const request_t &request, int width, int height, int framerate, Apply &&apply_legacy) {
    if (!managed) {
      std::forward<Apply>(apply_legacy)();
      return { decision_e::applied, {} };
    }

    std::ostringstream message;
    if (request.kind == kind_e::resolution) {
      if (request.width == width && request.height == height) {
        return { decision_e::unchanged, {} };
      }
      message << "Refusing dynamic resolution change on a managed native display: keeping "
              << width << 'x' << height << ", requested " << request.width << 'x' << request.height
              << " (managed head mode changes are not supported)";
    }
    else {
      if (request.rate == static_cast<float>(framerate)) {
        return { decision_e::unchanged, {} };
      }
      message << "Refusing dynamic FPS change on a managed native display: keeping "
              << framerate << " FPS, requested " << request.rate << " FPS (managed head mode changes are not supported)";
    }
    return { decision_e::refused_managed, message.str() };
  }

  /// Gate against a session monitor config (`display_exact`, `width`, `height`, `framerate`).
  template <class Monitor, class Apply>
  outcome_t
  gate(const Monitor &monitor, const request_t &request, Apply &&apply_legacy) {
    return gate(monitor.display_exact, request, monitor.width, monitor.height, monitor.framerate, std::forward<Apply>(apply_legacy));
  }
}  // namespace stream::dynamic_mode
