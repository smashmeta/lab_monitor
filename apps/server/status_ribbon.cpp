#include "status_ribbon.hpp"

#include <QEasingCurve>
#include <QFont>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QVBoxLayout>

#include <utility>

#include "lm/ui/theme.hpp"

namespace {
constexpr int kAnimationMs = 300;
}

StatusCounterButton::StatusCounterButton(QString title, QWidget* parent)
    : QWidget(parent),
      value_label_(new QLabel(QStringLiteral("0"), this)),
      title_label_(new QLabel(std::move(title), this)),
      animation_(new QVariantAnimation(this)) {
    setCursor(Qt::PointingHandCursor);

    QFont value_font = value_label_->font();
    value_font.setBold(true);
    value_font.setPointSize(value_font.pointSize() + 6);
    value_label_->setFont(value_font);
    value_label_->setAlignment(Qt::AlignCenter);
    title_label_->setAlignment(Qt::AlignCenter);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(value_label_);
    layout->addWidget(title_label_);

    animation_->setDuration(kAnimationMs);
    animation_->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        displayed_value_ = value.toInt();
        value_label_->setText(QString::number(displayed_value_));
    });

    update_style();
}

void StatusCounterButton::set_value(int value) {
    if (value == displayed_value_ && animation_->state() != QVariantAnimation::Running) {
        return;
    }
    animation_->stop();
    animation_->setStartValue(displayed_value_);
    animation_->setEndValue(value);
    animation_->start();
}

void StatusCounterButton::set_active(bool active) {
    if (active_ == active) {
        return;
    }
    active_ = active;
    update_style();
}

void StatusCounterButton::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        emit clicked();
    }
    QWidget::mousePressEvent(event);
}

void StatusCounterButton::update_style() {
    // The old active state was a bare surface-colour fill, which against the
    // dark background was almost invisible -- clicking appeared to do nothing
    // except make rows come and go. An active counter now carries the accent
    // colour on its border, its background and its number, so "this filter is
    // on" is legible at a glance and from across the room.
    if (active_) {
        setStyleSheet(QStringLiteral("QWidget { background-color: %1; border: 1px solid %2;"
                                     " border-radius: 6px; }")
                          .arg(lm::ui::Theme::kElevated, lm::ui::Theme::kAccent));
        value_label_->setStyleSheet(
            QStringLiteral("color: %1; font-weight: 700;").arg(lm::ui::Theme::kAccent));
        title_label_->setStyleSheet(QStringLiteral("color: %1;").arg(lm::ui::Theme::kAccent));
    } else {
        setStyleSheet(QStringLiteral("QWidget { background-color: transparent;"
                                     " border: 1px solid transparent; border-radius: 6px; }"));
        value_label_->setStyleSheet(QStringLiteral("color: %1;").arg(lm::ui::Theme::kText));
        title_label_->setStyleSheet(QStringLiteral("color: %1;").arg(lm::ui::Theme::kTextMuted));
    }

    setToolTip(active_
                   ? QStringLiteral("Showing only %1 hosts.\nClick again to show all.")
                         .arg(title_label_->text())
                   : QStringLiteral("Click to show only %1 hosts.").arg(title_label_->text()));
}

StatusRibbon::StatusRibbon(QWidget* parent)
    : QWidget(parent),
      online_(new StatusCounterButton(QStringLiteral("Online"), this)),
      offline_(new StatusCounterButton(QStringLiteral("Offline"), this)),
      missing_(new StatusCounterButton(QStringLiteral("Missing"), this)),
      unexpected_(new StatusCounterButton(QStringLiteral("Unexpected"), this)),
      stale_(new StatusCounterButton(QStringLiteral("Stale"), this)) {
    auto* layout = new QHBoxLayout(this);
    layout->addWidget(online_);
    layout->addWidget(offline_);
    layout->addWidget(missing_);
    layout->addWidget(unexpected_);
    layout->addWidget(stale_);
    layout->addStretch();

    connect(online_, &StatusCounterButton::clicked, this,
            [this] { handle_state_clicked(lm::core::HostState::Online); });
    connect(offline_, &StatusCounterButton::clicked, this,
            [this] { handle_state_clicked(lm::core::HostState::Offline); });
    connect(missing_, &StatusCounterButton::clicked, this,
            [this] { handle_state_clicked(lm::core::HostState::Missing); });
    connect(unexpected_, &StatusCounterButton::clicked, this,
            [this] { handle_state_clicked(lm::core::HostState::Unexpected); });
    connect(stale_, &StatusCounterButton::clicked, this, &StatusRibbon::handle_stale_clicked);
}

void StatusRibbon::set_counts(const lm::core::FleetCounts& counts) {
    online_->set_value(static_cast<int>(counts.online));
    offline_->set_value(static_cast<int>(counts.offline));
    missing_->set_value(static_cast<int>(counts.missing));
    unexpected_->set_value(static_cast<int>(counts.unexpected));
    stale_->set_value(static_cast<int>(counts.stale));
}

void StatusRibbon::clear_active() {
    active_state_filter_.reset();
    stale_filter_active_ = false;
    online_->set_active(false);
    offline_->set_active(false);
    missing_->set_active(false);
    unexpected_->set_active(false);
    stale_->set_active(false);
}

void StatusRibbon::handle_state_clicked(lm::core::HostState state) {
    if (active_state_filter_ == state) {
        active_state_filter_.reset();
    } else {
        active_state_filter_ = state;
    }

    online_->set_active(active_state_filter_ == lm::core::HostState::Online);
    offline_->set_active(active_state_filter_ == lm::core::HostState::Offline);
    missing_->set_active(active_state_filter_ == lm::core::HostState::Missing);
    unexpected_->set_active(active_state_filter_ == lm::core::HostState::Unexpected);

    emit filter_requested(active_state_filter_);
}

void StatusRibbon::handle_stale_clicked() {
    stale_filter_active_ = !stale_filter_active_;
    stale_->set_active(stale_filter_active_);
    emit stale_filter_requested(stale_filter_active_);
}
