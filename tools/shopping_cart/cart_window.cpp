#include "cart_window.hpp"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QGroupBox>

#include <spdlog/spdlog.h>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <utility>

#include "lm/ui/log_view.hpp"
#include "lm/ui/theme.hpp"

namespace {

QTableWidget* make_table(const QStringList& headers) {
    auto* table = new QTableWidget(0, headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    return table;
}

}  // namespace

CartWindow::CartWindow(std::uint32_t domain_id, QString topic_name, bool localhost_only,
                       QWidget* parent)
    : QMainWindow(parent) {
    // The scope is in the title because it changes what a rule against this
    // cart means. Confined, every PC answers "items_.length equal to 2" about
    // its own cart; open, they are all one bus and a client reads whichever it
    // discovers first -- and the two look identical from the server.
    setWindowTitle(QStringLiteral("Shopping Cart — domain %1, topic %2 — %3")
                       .arg(domain_id)
                       .arg(topic_name)
                       .arg(localhost_only ? QStringLiteral("this PC only")
                                           : QStringLiteral("whole network")));

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    banner_ = new QLabel(central);
    banner_->setWordWrap(true);
    layout->addWidget(banner_);

    // ---- adding ----
    auto* add_box = new QGroupBox(QStringLiteral("Add an item"), central);
    auto* add_row = new QHBoxLayout(add_box);
    sku_edit_ = new QLineEdit(add_box);
    sku_edit_->setPlaceholderText(QStringLiteral("SKU, e.g. A-100"));
    price_spin_ = new QDoubleSpinBox(add_box);
    price_spin_->setPrefix(QStringLiteral("price "));
    price_spin_->setRange(0.0, 100000.0);
    price_spin_->setValue(9.50);
    quantity_spin_ = new QSpinBox(add_box);
    quantity_spin_->setPrefix(QStringLiteral("qty "));
    quantity_spin_->setRange(1, 999);
    auto* add_button = new QPushButton(QStringLiteral("Add"), add_box);
    add_button->setDefault(true);

    add_row->addWidget(sku_edit_, 1);
    add_row->addWidget(price_spin_);
    add_row->addWidget(quantity_spin_);
    add_row->addWidget(add_button);
    layout->addWidget(add_box);

    // ---- the cart ----
    auto* cart_box = new QGroupBox(QStringLiteral("Cart"), central);
    auto* cart_layout = new QVBoxLayout(cart_box);
    items_table_ = make_table({QStringLiteral("SKU"), QStringLiteral("Price"),
                                QStringLiteral("Quantity"), QStringLiteral("Line total")});
    cart_layout->addWidget(items_table_, 1);

    auto* cart_row = new QHBoxLayout();
    auto* remove_button = new QPushButton(QStringLiteral("Remove selected"), cart_box);
    cart_row->addWidget(remove_button);
    cart_row->addStretch(1);
    cart_row->addWidget(new QLabel(QStringLiteral("Status:"), cart_box));
    status_box_ = new QComboBox(cart_box);
    status_box_->addItems({QStringLiteral("Ready"), QStringLiteral("Packing"),
                            QStringLiteral("Shipped"), QStringLiteral("Cancelled")});
    cart_row->addWidget(status_box_);
    cart_layout->addLayout(cart_row);
    layout->addWidget(cart_box, 1);

    // ---- what a rule can address ----
    //
    // The reason this pane exists: the server has never seen this type and
    // cannot offer its fields, so without somewhere to read the grammar an
    // operator is guessing. Copy a path from here into an
    // "DDS: value on a topic" rule and it addresses something real.
    auto* paths_box = new QGroupBox(
        QStringLiteral("Paths a DDS rule can address — copy one into Add Rule"), central);
    auto* paths_layout = new QVBoxLayout(paths_box);
    paths_table_ = make_table({QStringLiteral("Path"), QStringLiteral("Current value")});
    paths_table_->setMinimumHeight(150);
    // Monospace, and not for decoration: in the proportional UI font the
    // underscore and the dot of "items_.length" merge into one stroke and it
    // reads as "items_length" -- which addresses nothing, on the one path an
    // operator is most likely to copy.
    paths_table_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    paths_layout->addWidget(paths_table_);

    // The group title says "copy one", so Ctrl+C has to actually copy one.
    auto* copy = new QShortcut(QKeySequence::Copy, paths_table_);
    copy->setContext(Qt::WidgetShortcut);
    connect(copy, &QShortcut::activated, this, [this] {
        const int row = paths_table_->currentRow();
        if (row < 0 || paths_table_->item(row, 0) == nullptr) {
            return;
        }
        QApplication::clipboard()->setText(paths_table_->item(row, 0)->text());
        set_banner(QStringLiteral("Copied \"%1\" — paste it into Add Rule")
                       .arg(paths_table_->item(row, 0)->text()),
                   true);
    });

    layout->addWidget(paths_box);

    // A collapsible panel rather than a tab. The whole point of this window is
    // watching the cart and the effect of changing it together -- a tab would
    // hide the cart to show the log, which is the one arrangement that helps
    // nobody. Collapsed by default so the fixture still opens at its usual
    // size; the sink is attached either way, so expanding it shows everything
    // that happened while it was shut.
    auto* log_box = new QGroupBox(QStringLiteral("Activity log"), central);
    log_box->setCheckable(true);
    log_box->setChecked(false);
    auto* log_layout = new QVBoxLayout(log_box);
    log_view_ = new lm::ui::LogView(log_box);
    log_view_->attach_to_default_logger();
    log_view_->setVisible(false);
    log_layout->addWidget(log_view_);
    connect(log_box, &QGroupBox::toggled, log_view_, &QWidget::setVisible);
    layout->addWidget(log_box);

    setCentralWidget(central);

    connect(add_button, &QPushButton::clicked, this, &CartWindow::on_add_clicked);
    connect(sku_edit_, &QLineEdit::returnPressed, this, &CartWindow::on_add_clicked);
    connect(remove_button, &QPushButton::clicked, this, &CartWindow::on_remove_clicked);
    connect(status_box_, &QComboBox::currentTextChanged, this, &CartWindow::on_status_changed);

    const std::string error = publisher_.start(domain_id, topic_name.toStdString(), localhost_only);
    publishing_ = error.empty();
    if (publishing_) {
        spdlog::info("publishing {} on domain {}", topic_name.toStdString(), domain_id);
    } else {
        spdlog::error("not publishing: {}", error);
    }
    if (!publishing_) {
        // Said plainly and left on screen: a fixture that fails quietly sends
        // somebody hunting through the probe for a fault that is on this side.
        set_banner(QStringLiteral("Not publishing — %1").arg(QString::fromStdString(error)), false);
    }

    apply_and_publish();

    // Typing a SKU is the first and most repeated thing anyone does here, so
    // the caret starts in that field and on_add_clicked() puts it back after
    // every add. Without this the window opens with nothing focused and the
    // first keystrokes go nowhere.
    sku_edit_->setFocus();
}

void CartWindow::set_banner(const QString& text, bool ok) {
    banner_->setText(text);
    banner_->setStyleSheet(QStringLiteral("color: %1;")
                               .arg(ok ? lm::ui::Theme::kOnline : lm::ui::Theme::kMissing));
}

void CartWindow::on_add_clicked() {
    const QString sku = sku_edit_->text().trimmed();
    if (sku.isEmpty()) {
        return;
    }
    cart::add(state_, sku.toStdString(), price_spin_->value(), quantity_spin_->value());
    sku_edit_->clear();
    sku_edit_->setFocus();
    apply_and_publish();
}

void CartWindow::on_remove_clicked() {
    const int row = items_table_->currentRow();
    if (row < 0 || static_cast<std::size_t>(row) >= state_.items.size()) {
        return;
    }
    cart::remove(state_, state_.items[static_cast<std::size_t>(row)].sku);
    apply_and_publish();
}

void CartWindow::on_status_changed(const QString& status) {
    state_.status = status.toStdString();
    apply_and_publish();
}

void CartWindow::apply_and_publish() {
    items_table_->setRowCount(static_cast<int>(state_.items.size()));
    for (int row = 0; row < items_table_->rowCount(); ++row) {
        const cart::Item& item = state_.items[static_cast<std::size_t>(row)];
        items_table_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(item.sku)));
        items_table_->setItem(row, 1, new QTableWidgetItem(QStringLiteral("%1").arg(item.price, 0, 'f', 2)));
        items_table_->setItem(row, 2, new QTableWidgetItem(QString::number(item.quantity)));
        items_table_->setItem(
            row, 3,
            new QTableWidgetItem(
                QStringLiteral("%1").arg(item.price * item.quantity, 0, 'f', 2)));
    }
    items_table_->resizeColumnsToContents();

    const auto paths = cart::rule_paths(state_);
    paths_table_->setRowCount(static_cast<int>(paths.size()));
    for (int row = 0; row < paths_table_->rowCount(); ++row) {
        const auto& [path, value] = paths[static_cast<std::size_t>(row)];
        paths_table_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(path)));
        paths_table_->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(value)));
    }
    paths_table_->resizeColumnsToContents();

    if (!publishing_) {
        return;  // the banner already says why, and it must not be overwritten
    }
    if (publisher_.publish(state_)) {
        set_banner(QStringLiteral("Published — %1 line(s), %2 unit(s) on the bus")
                       .arg(state_.items.size())
                       .arg(cart::unit_count(state_)),
                   true);
    } else {
        set_banner(QStringLiteral("The write failed — the cart on screen is ahead of the bus"), false);
    }
}
