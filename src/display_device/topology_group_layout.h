/**
 * @file src/display_device/topology_group_layout.h
 * @brief The display group layout control message (0x55F0 v1, host to client).
 * @details Wire contract shared with moonlight-common-c (`docs/protocol-extension-registry.md`,
 *          `src/DisplayGroupStream.h`). Little-endian, exactly `20 + 24 * N` bytes:
 *          `u8 version=1, u8 headCount (1..8), u8 thisHead, u8 reserved=0, u8 groupId[16]`, then
 *          per head `u8 index, u8 admission, u16 reason, i32 desktopX, i32 desktopY, u32 width,
 *          u32 height, u32 refreshMilliHz`. Sent after PLAY to every session of a group, and
 *          again when a head changes; the latest message wins.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "topology_coordinator.h"

namespace display_device::topology {

  inline constexpr std::uint16_t display_group_layout_type = 0x55F0;
  inline constexpr std::uint8_t display_group_layout_version = 1;
  inline constexpr std::size_t display_group_layout_header = 20;
  inline constexpr std::size_t display_group_layout_entry = 24;

  /** The wire payload of `layout`; empty for a layout the contract cannot carry. */
  inline std::vector<std::uint8_t>
  encode_group_layout(const group_layout_t &layout) {
    const auto count = layout.heads.size();
    if (count == 0 || count > max_group_heads || layout.this_head >= count) {
      return {};
    }
    std::vector<std::uint8_t> wire(display_group_layout_header + display_group_layout_entry * count, 0);
    const auto put16 = [&wire](std::size_t at, std::uint16_t value) {
      wire[at] = static_cast<std::uint8_t>(value);
      wire[at + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    const auto put32 = [&wire](std::size_t at, std::uint32_t value) {
      for (std::size_t i = 0; i < 4; ++i) {
        wire[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
      }
    };
    wire[0] = display_group_layout_version;
    wire[1] = static_cast<std::uint8_t>(count);
    wire[2] = layout.this_head;
    std::copy(layout.id.begin(), layout.id.end(), wire.begin() + 4);
    for (std::size_t h = 0; h < count; ++h) {
      const auto at = display_group_layout_header + display_group_layout_entry * h;
      const auto &head = layout.heads[h];
      const auto geometry = head.geometry.value_or(head_geometry_t {});
      wire[at] = static_cast<std::uint8_t>(h);
      wire[at + 1] = static_cast<std::uint8_t>(head.state.admission);
      put16(at + 2, static_cast<std::uint16_t>(head.state.reason));
      put32(at + 4, static_cast<std::uint32_t>(geometry.x));
      put32(at + 8, static_cast<std::uint32_t>(geometry.y));
      put32(at + 12, geometry.width);
      put32(at + 16, geometry.height);
      put32(at + 20, geometry.refresh_millihz);
    }
    return wire;
  }

  /**
   * Per session: sends each group layout revision at most once. `due` returns the payload when
   * the revision moved and the session has a layout; call `delivered` after it was sent, so a
   * failed send is retried on the next poll.
   */
  class group_layout_sender_t {
  public:
    std::optional<std::vector<std::uint8_t>>
    due(std::uint64_t revision, const std::function<std::optional<group_layout_t>()> &snapshot) {
      if (revision == seen_) {
        return std::nullopt;
      }
      const auto layout = snapshot();
      auto wire = layout ? encode_group_layout(*layout) : std::vector<std::uint8_t> {};
      if (wire.empty()) {
        // Not a group session (or nothing the contract can carry) at this revision: ask again only when it moves.
        seen_ = revision;
        return std::nullopt;
      }
      return wire;
    }

    void
    delivered(std::uint64_t revision) noexcept {
      seen_ = revision;
    }

  private:
    std::uint64_t seen_ {};
  };

}  // namespace display_device::topology
