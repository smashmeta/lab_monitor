#include "lm/ui/meter_bar.hpp"

#include <algorithm>

#include <QColor>
#include <QEasingCurve>
#include <QPainter>
#include <QPainterPath>
#include <QString>
#include <QVariantAnimation>

#include "lm/ui/theme.hpp"

namespace lm::ui {

namespace {
constexpr int kAnimationMs = 300;
}

MeterBar::MeterBar(QWidget* parent) : QWidget(parent), animation_(new QVariantAnimation(this)) {
    animation_->setDuration(kAnimationMs);
    animation_->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        displayed_value_ = value.toDouble();
        update();
    });
}

void MeterBar::set_value(double percent) {
    const double target = std::clamp(percent, 0.0, 100.0);
    animation_->stop();
    animation_->setStartValue(displayed_value_);
    animation_->setEndValue(target);
    animation_->start();
}

QSize MeterBar::sizeHint() const {
    return QSize(120, 14);
}

void MeterBar::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF bounds = rect().adjusted(0, 0, -1, -1);
    const double radius = bounds.height() / 2.0;

    QPainterPath track;
    track.addRoundedRect(bounds, radius, radius);
    painter.fillPath(track, QColor(Theme::kSurface));

    const double fraction = displayed_value_ / 100.0;
    QRectF fill_rect = bounds;
    fill_rect.setWidth(bounds.width() * fraction);
    if (fill_rect.width() > 0.0) {
        QPainterPath fill;
        fill.addRoundedRect(fill_rect, radius, radius);
        const QColor color =
            displayed_value_ >= 90.0 ? QColor(Theme::kMissing) : QColor(Theme::kAccent);
        painter.fillPath(fill, color);
    }

    painter.setPen(QColor(Theme::kText));
    painter.drawText(bounds, Qt::AlignCenter, QString::number(displayed_value_, 'f', 0) + "%");
}

}  // namespace lm::ui
