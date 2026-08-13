#include "lm/ui/theme.hpp"

#include <QApplication>
#include <QFile>
#include <QIODevice>

#include <spdlog/spdlog.h>

#include "lm/core/fleet.hpp"
#include "lm/core/types.hpp"

/// lm_ui is a STATIC library, so AUTORCC's generated qrc translation unit
/// (compiled into the archive) is never referenced by anything and the
/// linker drops it entirely -- nothing in either app's link line pulls in
/// its file-static initializer. Q_INIT_RESOURCE forces that reference. It
/// expands to a block-scope `extern int qInitResources_lm_ui();` declaration
/// followed by a call to it, so this wrapper must live at global scope, not
/// inside namespace lm::ui::Theme: called from in there, the extern would
/// instead resolve to (and fail to link) lm::ui::Theme::qInitResources_lm_ui.
/// `static` (rather than an unnamed namespace) gives this file-local linkage
/// without also touching the linkage MSVC assigns to Q_INIT_RESOURCE's own
/// nested extern -- wrapping this in `namespace { ... }` makes MSVC treat
/// that nested extern as internal-linkage too (warning C5046, fatal under
/// this project's /W4 warnings-as-errors), since the whole function is then
/// syntactically nested inside the unnamed namespace.
static void lm_ui_init_resources() { Q_INIT_RESOURCE(lm_ui); }

namespace lm::ui {
namespace Theme {

void apply(QApplication& app) {
    ::lm_ui_init_resources();

    QFile file(QStringLiteral(":/lm_ui/theme.qss"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(QString::fromUtf8(file.readAll()));
    } else {
        spdlog::error("lm::ui::Theme::apply: failed to open :/lm_ui/theme.qss -- styling will be unset");
    }
}

QColor color_for(core::HostState state) {
    switch (state) {
        case core::HostState::Online:     return QColor(kOnline);
        case core::HostState::Offline:    return QColor(kOffline);
        case core::HostState::Missing:    return QColor(kMissing);
        case core::HostState::Unexpected: return QColor(kUnexpected);
    }
    return QColor(kNotApplicable);
}

QColor color_for(core::CheckStatus status) {
    switch (status) {
        case core::CheckStatus::Pass:         return QColor(kOnline);
        case core::CheckStatus::Fail:         return QColor(kMissing);
        case core::CheckStatus::NotApplicable: return QColor(kNotApplicable);
        case core::CheckStatus::Error:        return QColor(kUnexpected);
    }
    return QColor(kNotApplicable);
}

QString glyph_for(core::HostState state) {
    switch (state) {
        case core::HostState::Online:     return QStringLiteral("✓");  // check mark
        case core::HostState::Offline:    return QStringLiteral("!");
        case core::HostState::Missing:    return QStringLiteral("✕");  // multiplication x
        case core::HostState::Unexpected: return QStringLiteral("?");
    }
    return QStringLiteral("◌");  // dotted circle
}

QString glyph_for(core::CheckStatus status) {
    switch (status) {
        case core::CheckStatus::Pass:         return QStringLiteral("✓");
        case core::CheckStatus::Fail:         return QStringLiteral("!");
        case core::CheckStatus::NotApplicable: return QStringLiteral("◌");
        case core::CheckStatus::Error:        return QStringLiteral("?");
    }
    return QStringLiteral("◌");
}

}  // namespace Theme
}  // namespace lm::ui
