#include "lm/ui/adapter_list.hpp"

#include <QColor>
#include <QHeaderView>
#include <QTreeWidgetItem>

#include <algorithm>

#include "lm/ui/keep_foreground_delegate.hpp"
#include "lm/ui/theme.hpp"

namespace lm::ui {

AdapterList::AdapterList(QWidget* parent) : QTreeWidget(parent) {
    setColumnCount(4);
    setHeaderLabels({tr("Adapter"), tr("Description"), tr("Type"), tr("Link")});
    setRootIsDecorated(false);
    setUniformRowHeights(true);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    // Keeps the up/down colouring when a row is selected, as everywhere else.
    setItemDelegate(new KeepForegroundDelegate(this));
    header()->setSectionResizeMode(1, QHeaderView::Stretch);
}

void AdapterList::set_not_reported() {
    clear();
    auto* row = new QTreeWidgetItem(this);
    row->setText(0, tr("Not reported"));
    row->setText(1, tr("This client does not report network adapters."));
    row->setFirstColumnSpanned(false);
    for (int column = 0; column < columnCount(); ++column) {
        row->setForeground(column, QColor(Theme::kTextMuted));
    }
}

void AdapterList::set_adapters(const std::vector<core::NetworkAdapter>& adapters) {
    clear();

    std::vector<core::NetworkAdapter> ordered = adapters;
    // Connected first, then by description. Sorting on the description rather
    // than the name because the name is a GUID on Windows and sorts as noise.
    std::ranges::sort(ordered, [](const core::NetworkAdapter& lhs, const core::NetworkAdapter& rhs) {
        if (lhs.connected != rhs.connected) {
            return lhs.connected;
        }
        return lhs.description < rhs.description;
    });

    for (const core::NetworkAdapter& adapter : ordered) {
        auto* row = new QTreeWidgetItem(this);
        row->setText(0, QString::fromStdString(adapter.name));
        row->setText(1, QString::fromStdString(adapter.description));
        row->setText(2, QString::fromStdString(core::to_string(adapter.type)));
        // Glyph as well as colour, so link state survives greyscale — the same
        // rule the status pills and compliance rows follow.
        row->setText(3, adapter.connected ? tr("✓ Up") : tr("✕ Down"));

        const QColor colour(adapter.connected ? Theme::kOnline : Theme::kTextMuted);
        const QString tooltip = tr("%1\n%2\n\nType:\t%3\nLink:\t%4\nId:\t%5")
                                    .arg(QString::fromStdString(adapter.name),
                                          QString::fromStdString(adapter.description),
                                          QString::fromStdString(core::to_string(adapter.type)),
                                          adapter.connected ? tr("connected") : tr("not connected"),
                                          QString::fromStdString(adapter.id));
        for (int column = 0; column < columnCount(); ++column) {
            row->setForeground(column, colour);
            row->setToolTip(column, tooltip);
        }
    }

    resizeColumnToContents(0);
    // Capped so one long name cannot swallow the description's width.
    constexpr int kMaxNameWidth = 190;
    setColumnWidth(0, std::min(columnWidth(0), kMaxNameWidth));
    resizeColumnToContents(2);
    resizeColumnToContents(3);
}

}  // namespace lm::ui
