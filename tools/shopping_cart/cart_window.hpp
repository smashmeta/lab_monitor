#pragma once

#include <QMainWindow>
#include <QString>

#include <cstdint>

namespace lm::ui {
class LogView;
}  // namespace lm::ui

#include "cart_publisher.hpp"
#include "cart_state.hpp"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTableWidget;

/// The whole tool: edit a cart, and every edit goes onto the bus.
///
/// Publishing on change rather than on a timer is the point. Someone adds an
/// item here and watches a rule on the server flip within the client's next
/// evaluation, which is the loop this exists to make walkable.
class CartWindow : public QMainWindow {
    Q_OBJECT

public:
    CartWindow(std::uint32_t domain_id, QString topic_name, bool localhost_only,
               QWidget* parent = nullptr);

private slots:
    void on_add_clicked();
    void on_remove_clicked();
    void on_status_changed(const QString& status);

private:
    /// Repaints the table and the path list, then writes. One function because
    /// the three must never disagree: the paths pane is what an operator copies
    /// a rule from, so it has to show what was actually published.
    void apply_and_publish();
    void set_banner(const QString& text, bool ok);

    cart::State state_;
    cart::Publisher publisher_;
    bool publishing_ = false;

    QLineEdit* sku_edit_;
    QDoubleSpinBox* price_spin_;
    QSpinBox* quantity_spin_;
    QComboBox* status_box_;
    QTableWidget* items_table_;
    QTableWidget* paths_table_;
    QLabel* banner_;
    lm::ui::LogView* log_view_ = nullptr;
};
