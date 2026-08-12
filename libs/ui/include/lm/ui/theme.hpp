#pragma once

#include <QColor>
#include <QString>

#include "lm/core/fleet.hpp"
#include "lm/core/types.hpp"

class QApplication;

namespace lm::ui {

/// Dark slate with a single cyan accent. Status is always colour *and* glyph,
/// never colour alone, so it survives greyscale and colour-blindness.
namespace Theme {

inline constexpr const char* kAccent = "#22d3ee";
inline constexpr const char* kBackground = "#0f172a";
inline constexpr const char* kSurface = "#1e293b";
inline constexpr const char* kText = "#e2e8f0";
inline constexpr const char* kTextMuted = "#94a3b8";

inline constexpr const char* kOnline = "#34d399";
inline constexpr const char* kOffline = "#fbbf24";
inline constexpr const char* kMissing = "#f87171";
inline constexpr const char* kUnexpected = "#a78bfa";
inline constexpr const char* kNotApplicable = "#64748b";

void apply(QApplication& app);

[[nodiscard]] QColor color_for(core::HostState state);
[[nodiscard]] QColor color_for(core::CheckStatus status);

/// A distinct shape per state, so hue is never the only signal.
[[nodiscard]] QString glyph_for(core::HostState state);
[[nodiscard]] QString glyph_for(core::CheckStatus status);

}  // namespace Theme
}  // namespace lm::ui
