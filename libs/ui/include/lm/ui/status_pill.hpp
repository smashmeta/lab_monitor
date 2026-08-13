#pragma once

#include <QSize>
#include <QWidget>

#include "lm/core/fleet.hpp"

namespace lm::ui {

/// A small rounded-rect badge showing a host's state as colour, glyph and
/// caption text together, so the state reads correctly in greyscale too.
class StatusPill : public QWidget {
    Q_OBJECT

public:
    explicit StatusPill(QWidget* parent = nullptr);

    void set_state(core::HostState state);
    [[nodiscard]] core::HostState state() const { return state_; }

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    core::HostState state_ = core::HostState::Missing;
};

}  // namespace lm::ui
