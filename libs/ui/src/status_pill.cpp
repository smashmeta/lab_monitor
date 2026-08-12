#include "lm/ui/status_pill.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QString>

#include "lm/ui/theme.hpp"

namespace lm::ui {

StatusPill::StatusPill(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground);
}

void StatusPill::set_state(core::HostState state) {
    if (state_ == state) {
        return;
    }
    state_ = state;
    update();
}

QSize StatusPill::sizeHint() const {
    const QFontMetrics metrics(font());
    const QString caption = QString::fromStdString(core::to_string(state_));
    const int text_width = metrics.horizontalAdvance(caption);
    return QSize(text_width + 48, metrics.height() + 12);
}

void StatusPill::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QColor color = Theme::color_for(state_);
    const QRectF bounds = rect().adjusted(1, 1, -1, -1);

    QPainterPath path;
    path.addRoundedRect(bounds, bounds.height() / 2.0, bounds.height() / 2.0);
    painter.fillPath(path, color);

    painter.setPen(Qt::black);
    const QString glyph = Theme::glyph_for(state_);
    const QString caption = QString::fromStdString(core::to_string(state_));
    painter.drawText(bounds, Qt::AlignCenter, glyph + QStringLiteral("  ") + caption);
}

}  // namespace lm::ui
