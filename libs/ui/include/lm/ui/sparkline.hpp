#pragma once

#include <QSize>
#include <QVector>
#include <QWidget>

namespace lm::ui {

/// A minimal rolling line chart: push() appends a sample to a fixed-capacity
/// ring buffer and the widget repaints itself. No QtCharts dependency.
class Sparkline : public QWidget {
    Q_OBJECT

public:
    explicit Sparkline(QWidget* parent = nullptr);
    explicit Sparkline(int capacity, QWidget* parent = nullptr);

    /// Appends a sample, discarding the oldest once at capacity.
    void push(double value);

    void clear();

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    int capacity_;
    QVector<double> samples_;
};

}  // namespace lm::ui
