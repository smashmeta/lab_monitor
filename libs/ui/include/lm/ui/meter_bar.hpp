#pragma once

#include <QSize>
#include <QWidget>

class QVariantAnimation;

namespace lm::ui {

/// A horizontal percentage bar that glides to new values via QVariantAnimation
/// instead of snapping, so resource updates read as motion rather than flicker.
class MeterBar : public QWidget {
    Q_OBJECT

public:
    explicit MeterBar(QWidget* parent = nullptr);

    /// Animates the displayed value toward percent (clamped to [0, 100]) over
    /// 300 ms with an OutCubic easing curve.
    void set_value(double percent);

    [[nodiscard]] double value() const { return displayed_value_; }

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVariantAnimation* animation_;
    double displayed_value_ = 0.0;
};

}  // namespace lm::ui
