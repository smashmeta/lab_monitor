#pragma once

#include <QLabel>
#include <QVariantAnimation>
#include <QWidget>

#include <optional>

#include "lm/core/fleet.hpp"

/// A single clickable counter inside StatusRibbon: a title, a value that
/// glides to its new number via QVariantAnimation, and a clicked() signal.
/// Declared here (rather than nested inside StatusRibbon or defined only in
/// status_ribbon.cpp) so moc -- which AUTOMOC only reliably runs against
/// Q_OBJECT types declared in a header sharing the target's source list --
/// picks it up without needing a manual "#include *.moc" trick.
class StatusCounterButton : public QWidget {
    Q_OBJECT

public:
    explicit StatusCounterButton(QString title, QWidget* parent = nullptr);

    /// Animates the displayed number toward value over 300 ms.
    void set_value(int value);

    void set_active(bool active);
    [[nodiscard]] bool active() const { return active_; }

signals:
    void clicked();

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    void update_style();

    QLabel* value_label_;
    QLabel* title_label_;
    QVariantAnimation* animation_;
    int displayed_value_ = 0;
    bool active_ = false;
};

/// A horizontal row of clickable counters: Online, Offline, Missing,
/// Unexpected, Stale. Clicking a state counter toggles a state filter;
/// clicking the active one again clears it. Stale is not a lm::core::HostState
/// (it is an orthogonal flag on FleetEntry), so it is exposed through its own
/// stale_filter_requested(bool) signal rather than being folded into
/// filter_requested's std::optional<HostState>.
class StatusRibbon : public QWidget {
    Q_OBJECT

public:
    explicit StatusRibbon(QWidget* parent = nullptr);

    void set_counts(const lm::core::FleetCounts& counts);

signals:
    void filter_requested(std::optional<lm::core::HostState> state);
    void stale_filter_requested(bool active);

private:
    void handle_state_clicked(lm::core::HostState state);
    void handle_stale_clicked();

    StatusCounterButton* online_;
    StatusCounterButton* offline_;
    StatusCounterButton* missing_;
    StatusCounterButton* unexpected_;
    StatusCounterButton* stale_;

    std::optional<lm::core::HostState> active_state_filter_;
    bool stale_filter_active_ = false;
};
