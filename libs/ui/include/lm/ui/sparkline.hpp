#pragma once

#include "lm/ui/export.hpp"

#include <QColor>
#include <QSize>
#include <QVector>
#include <QWidget>

#include <functional>

namespace lm::ui {

/// A minimal rolling line chart: push() appends a sample to a fixed-capacity
/// ring buffer and the widget repaints itself. No QtCharts dependency.
class LM_UI_EXPORT Sparkline : public QWidget {
    Q_OBJECT

public:
    explicit Sparkline(QWidget* parent = nullptr);
    explicit Sparkline(int capacity, QWidget* parent = nullptr);

    /// Appends a sample, discarding the oldest once at capacity.
    void push(double value);

    void clear();

    /// The line's colour; the area under it is the same hue, translucent.
    /// Set by the caller rather than defaulted to a metric-specific colour,
    /// so this stays a plain line chart rather than a CPU widget.
    void set_color(const QColor& color);
    [[nodiscard]] QColor color() const { return color_; }

    /// Recolours the line from each new sample, so the chart carries the
    /// current reading as well as its shape.
    ///
    /// A mapping rather than a per-push call at each site: forgetting it in one
    /// of them would leave that chart stuck on a colour it once had, which
    /// looks like data rather than like a bug. Pass {} to go back to a fixed
    /// colour. What the numbers *mean* stays the caller's business.
    void set_color_ramp(std::function<QColor(double)> ramp);

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    int capacity_;
    QVector<double> samples_;
    QColor color_;
    std::function<QColor(double)> ramp_;
};

}  // namespace lm::ui
