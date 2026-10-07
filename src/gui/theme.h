#pragma once

#include <cstdint>

namespace ir::gui::theme {

// UniyaOver dark monochrome palette (mockup-inspired).
inline constexpr uint32_t kBg = 0xFF0B0B0B;
inline constexpr uint32_t kSidebar = 0xFF101010;
inline constexpr uint32_t kPanel = 0xFF141414;
inline constexpr uint32_t kCard = 0xFF1A1A1A;
inline constexpr uint32_t kCardHover = 0xFF222222;
inline constexpr uint32_t kNavActive = 0xFF1E1E1E;
inline constexpr uint32_t kBorder = 0xFF2A2A2A;
inline constexpr uint32_t kText = 0xFFF2F2F2;
inline constexpr uint32_t kTextDim = 0xFF9A9A9A;
inline constexpr uint32_t kTextMuted = 0xFF6E6E6E;
inline constexpr uint32_t kAccent = 0xFFE8E8E8;
inline constexpr uint32_t kThumb = 0xFF2C2C2C;
inline constexpr uint32_t kStatusIdle = 0xFF7A7A7A;
inline constexpr uint32_t kStatusLive = 0xFFD0D0D0;

inline constexpr int kSidebarW = 200;
inline constexpr int kRightPanelW = 320;
inline constexpr int kPad = 20;
inline constexpr int kRadius = 12;
inline constexpr int kNavH = 40;
inline constexpr int kClipRowH = 72;

}  // namespace ir::gui::theme
