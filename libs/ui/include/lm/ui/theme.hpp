#pragma once

#include "lm/ui/export.hpp"

#include <QColor>
#include <QString>

#include "lm/core/fleet.hpp"
#include "lm/core/types.hpp"

class QApplication;

namespace lm::ui {

/// Dark slate with a single cyan accent. Status is always colour *and* glyph,
/// never colour alone, so it survives greyscale and colour-blindness.
namespace Theme {

// Chrome's dark-mode palette. Chosen over an invented one so the window sits
// alongside a dark browser without clashing, and because these values are
// already contrast-checked against each other by Google.
inline constexpr const char* kBackground = "#202124";  ///< window / base
inline constexpr const char* kSurface = "#292a2d";     ///< toolbars, headers, inputs
inline constexpr const char* kElevated = "#35363a";    ///< hover, selected tab
inline constexpr const char* kBorder = "#3c4043";      ///< dividers, outlines
inline constexpr const char* kText = "#e8eaed";        ///< primary text
inline constexpr const char* kTextMuted = "#9aa0a6";   ///< secondary text
inline constexpr const char* kAccent = "#8ab4f8";      ///< Chrome's blue
inline constexpr const char* kAccentPressed = "#aecbfa";


// Status colours from the same family, so they read as part of one palette
// rather than as warning lights bolted on.
inline constexpr const char* kOnline = "#81c995";
inline constexpr const char* kOffline = "#fdd663";
inline constexpr const char* kMissing = "#f28b82";
inline constexpr const char* kUnexpected = "#c58af9";
inline constexpr const char* kNotApplicable = "#9aa0a6";

LM_UI_EXPORT void apply(QApplication& app);

[[nodiscard]] LM_UI_EXPORT QColor color_for(core::HostState state);
[[nodiscard]] LM_UI_EXPORT QColor color_for(core::CheckStatus status);

/// A load reading as a colour: green at 0%, amber through the middle, red at
/// 100%, interpolated between the palette's own status colours so nothing
/// foreign to the window appears. Out-of-range values clamp.
///
/// Note this deliberately spends the status hues on a *measurement*. It reads
/// naturally — idle is green, saturated is red — but it does mean a red CPU
/// line and a red Missing host are the same colour meaning different things,
/// which is why the line stays inside a pane already labelled "CPU".
[[nodiscard]] LM_UI_EXPORT QColor color_for_load(double percent);

/// A distinct shape per state, so hue is never the only signal.
[[nodiscard]] LM_UI_EXPORT QString glyph_for(core::HostState state);
[[nodiscard]] LM_UI_EXPORT QString glyph_for(core::CheckStatus status);

}  // namespace Theme
}  // namespace lm::ui
