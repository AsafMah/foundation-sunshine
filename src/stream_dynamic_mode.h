/**
 * @file src/stream_dynamic_mode.h
 * @brief Client-driven resolution/FPS change requests (provisional pending G1 hardware).
 *
 * Legacy sessions run the unchanged legacy handler. A managed native head is
 * admitted, prepared and captured at one exact mode, so a managed session only
 * changes resolution through `change_managed_resolution`: the new mode is admitted
 * while the old admission stays held (make-before-break), then the topology owner
 * changes the head's mode, and only then does the session follow. Any refusal or
 * failure keeps (or restores) the old mode. Managed FPS changes stay refused.
 */
#pragma once

#include <cstdint>
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
    applied,  ///< The legacy handler ran, or a managed head now runs the requested mode.
    unchanged,  ///< Managed session asked for its current mode; nothing to do.
    refused_managed,  ///< Managed session: refused; the old mode is kept (INFO).
    failed,  ///< Managed session: the previous mode could not be proven after the head was touched; the stream must stop (WARN).
  };

  struct outcome_t {
    decision_e decision;
    std::string message;  ///< Log line for a managed outcome; empty for legacy and unchanged.
  };

  /** The topology owner's answer to a managed head mode change. */
  enum class head_verdict_e {
    applied,
    unchanged,
    refused,  ///< Nothing changed; the head keeps its mode.
    conflict,  ///< The head's state is uncertain.
  };

  struct head_change_t {
    head_verdict_e verdict = head_verdict_e::refused;
    std::string display;  ///< The head's display afterwards; empty when unknown.
    std::string reason;
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

  inline constexpr int max_resolution = 16384;

  /**
   * Change a managed session's resolution from @p width x @p height (on @p display) to the request.
   *
   * - `run_exclusive(body) -> bool`: runs `body` only while this is the only video session.
   * - `reserve(w, h) -> std::expected<P, std::string>`: admits the new mode while the old one stays held.
   * - `change(w, h) -> head_change_t`: the topology owner's head mode change.
   * - `commit(P &&) -> std::expected<void, std::string>`: swaps the session's admission to the new mode.
   * - `apply(w, h)`: the session follows (config, perf, IDR).
   *
   * Nothing past a refusal runs. Once the head may have changed, any failure (conflict, a moved head, a refused
   * commit) changes it back to the old mode on the same display; if that cannot be proven the outcome is `failed`.
   */
  template <class Exclusive, class Reserve, class Change, class Commit, class Apply>
  outcome_t
  change_managed_resolution(const request_t &request, int width, int height, const std::string &display,
    Exclusive &&run_exclusive, Reserve &&reserve, Change &&change, Commit &&commit, Apply &&apply) {
    const auto mode = [](int w, int h) {
      return std::to_string(w) + 'x' + std::to_string(h);
    };
    const auto keeping = "Refusing dynamic resolution change on a managed native display: keeping " + mode(width, height) +
                         ", requested " + mode(request.width, request.height) + " (";
    if (request.width <= 0 || request.width > max_resolution || request.height <= 0 || request.height > max_resolution) {
      return { decision_e::refused_managed, keeping + "invalid size)" };
    }
    if (request.width == width && request.height == height) {
      return { decision_e::unchanged, {} };
    }

    outcome_t outcome { decision_e::refused_managed, keeping + "other streams are active)" };
    std::forward<Exclusive>(run_exclusive)([&]() {
      const auto w = static_cast<std::uint32_t>(request.width);
      const auto h = static_cast<std::uint32_t>(request.height);
      auto pending = reserve(w, h);
      if (!pending) {
        outcome = { decision_e::refused_managed, keeping + "admission: " + pending.error() + ")" };
        return;
      }
      const auto changed = change(w, h);
      if (changed.verdict == head_verdict_e::refused) {
        outcome = { decision_e::refused_managed, keeping + "head: " + changed.reason + ")" };
        return;
      }
      // The head may have changed: every failure from here on must prove the
      // previous mode on the same display, or the stream cannot continue.
      const auto restore = [&](const std::string &why) {
        const auto restored = change(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
        if ((restored.verdict == head_verdict_e::applied || restored.verdict == head_verdict_e::unchanged) && restored.display == display) {
          outcome = { decision_e::refused_managed, keeping + why + "; previous mode restored)" };
        }
        else {
          outcome = { decision_e::failed, "Managed native display mode change to " + mode(request.width, request.height) + " failed (" + why +
                                            ") and the previous mode was not restored" +
                                            (restored.reason.empty() ? std::string {} : ": " + restored.reason) };
        }
      };
      if (changed.verdict == head_verdict_e::conflict) {
        restore("head: " + changed.reason);
        return;
      }
      if (changed.display != display) {
        restore("the head moved from [" + display + "] to [" + changed.display + "]; the stream cannot follow it yet");
        return;
      }
      if (auto committed = commit(std::move(*pending)); !committed) {
        restore("admission: " + committed.error());
        return;
      }
      apply(request.width, request.height);
      outcome = { decision_e::applied, "Managed native display mode changed: " + mode(width, height) + " -> " + mode(request.width, request.height) };
    });
    return outcome;
  }
}  // namespace stream::dynamic_mode
