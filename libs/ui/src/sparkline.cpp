#include "lm/ui/sparkline.hpp"

#include <algorithm>

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include "lm/ui/theme.hpp"

namespace lm::ui {

namespace {
constexpr int kDefaultCapacity = 60;
}

Sparkline::Sparkline(QWidget* parent) : Sparkline(kDefaultCapacity, parent) {}

Sparkline::Sparkline(int capacity, QWidget* parent)
    : QWidget(parent), capacity_(capacity > 1 ? capacity : kDefaultCapacity) {
    samples_.reserve(capacity_);
}

void Sparkline::push(double value) {
    samples_.append(value);
    while (samples_.size() > capacity_) {
        samples_.removeFirst();
    }
    update();
}

void Sparkline::clear() {
    samples_.clear();
    update();
}

QSize Sparkline::sizeHint() const {
    return QSize(160, 40);
}

void Sparkline::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    if (samples_.size() < 2) {
        return;
    }

    const auto [min_it, max_it] = std::minmax_element(samples_.begin(), samples_.end());
    double min_value = *min_it;
    double max_value = *max_it;
    if (max_value - min_value < 1e-9) {
        min_value -= 1.0;
        max_value += 1.0;
    }

    const QRectF bounds = rect();
    const double step = bounds.width() / static_cast<double>(capacity_ - 1);
    const double left_pad = bounds.width() - step * static_cast<double>(samples_.size() - 1);

    QPainterPath line;
    for (int i = 0; i < samples_.size(); ++i) {
        const double x = left_pad + step * static_cast<double>(i);
        const double t = (samples_[i] - min_value) / (max_value - min_value);
        const double y = bounds.bottom() - t * bounds.height();
        if (i == 0) {
            line.moveTo(x, y);
        } else {
            line.lineTo(x, y);
        }
    }

    QPainterPath fill = line;
    fill.lineTo(bounds.right(), bounds.bottom());
    fill.lineTo(left_pad, bounds.bottom());
    fill.closeSubpath();

    QColor accent(Theme::kAccent);
    QColor fill_color = accent;
    fill_color.setAlpha(40);
    painter.fillPath(fill, fill_color);

    painter.setPen(QPen(accent, 1.5));
    painter.drawPath(line);
}

}  // namespace lm::ui
