#include "lm/ui/theme.hpp"

#include <QApplication>
#include <QFile>
#include <QIODevice>

#include "lm/core/fleet.hpp"
#include "lm/core/types.hpp"

namespace lm::ui {
namespace Theme {

void apply(QApplication& app) {
    QFile file(QStringLiteral(":/lm_ui/theme.qss"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(QString::fromUtf8(file.readAll()));
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
