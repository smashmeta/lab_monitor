#include "lm/ui/adapter_list.hpp"

#include <QColor>
#include <QHeaderView>
#include <QTreeWidgetItem>

#include <algorithm>

#include "lm/ui/keep_foreground_delegate.hpp"
#include "lm/ui/theme.hpp"

namespace lm::ui {
namespace {

/// A shape per state, so link status is never carried by hue alone.
QString glyph_for(core::LinkState state) {
    switch (state) {
        case core::LinkState::Connected:    return QStringLiteral("✓");
        case core::LinkState::NoMedia:      return QStringLiteral("✕");
        case core::LinkState::Faulted:      return QStringLiteral("!");
        case core::LinkState::Connecting:   return QStringLiteral("…");
        case core::LinkState::Disabled:     return QStringLiteral("○");
        case core::LinkState::Disconnected: return QStringLiteral("–");
        case core::LinkState::Unknown:      break;
    }
    return QStringLiteral("?");
}

QColor colour_for(core::LinkState state) {
    switch (state) {
        case core::LinkState::Connected:  return QColor(Theme::kOnline);
        // Amber, not grey: an enabled adapter with nothing plugged into it is
        // a fault someone can fix, unlike one that is simply idle.
        case core::LinkState::NoMedia:    return QColor(Theme::kOffline);
        case core::LinkState::Faulted:    return QColor(Theme::kMissing);
        case core::LinkState::Connecting: return QColor(Theme::kAccent);
        default:                          return QColor(Theme::kTextMuted);
    }
}

/// The tooltip's sentence, which says what to do about it rather than just
/// naming the state again.
QString explain(core::LinkState state) {
    switch (state) {
        case core::LinkState::Connected:    return AdapterList::tr("connected");
        case core::LinkState::NoMedia:      return AdapterList::tr("enabled, but nothing connected to it");
        case core::LinkState::Disconnected: return AdapterList::tr("not connected");
        case core::LinkState::Connecting:   return AdapterList::tr("connecting");
        case core::LinkState::Disabled:     return AdapterList::tr("disabled");
        case core::LinkState::Faulted:      return AdapterList::tr("absent, faulty, or refusing to authenticate");
        case core::LinkState::Unknown:      break;
    }
    return AdapterList::tr("unknown");
}

}  // namespace

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
    // Up first, then whatever needs attention, then the merely idle — a
    // machine with twenty interfaces is usually being read for "what is wrong".
    const auto rank = [](core::LinkState state) {
        switch (state) {
            case core::LinkState::Connected:    return 0;
            case core::LinkState::Faulted:      return 1;
            case core::LinkState::NoMedia:      return 2;
            case core::LinkState::Connecting:   return 3;
            case core::LinkState::Disconnected: return 4;
            case core::LinkState::Disabled:     return 5;
            case core::LinkState::Unknown:      return 6;
        }
        return 7;
    };
    std::ranges::sort(ordered, [&](const core::NetworkAdapter& lhs, const core::NetworkAdapter& rhs) {
        if (rank(lhs.link) != rank(rhs.link)) {
            return rank(lhs.link) < rank(rhs.link);
        }
        return lhs.name < rhs.name;
    });

    for (const core::NetworkAdapter& adapter : ordered) {
        auto* row = new QTreeWidgetItem(this);
        row->setText(0, QString::fromStdString(adapter.name));
        row->setText(1, QString::fromStdString(adapter.description));
        row->setText(2, QString::fromStdString(core::to_string(adapter.type)));
        // Glyph as well as colour, so link state survives greyscale — the same
        // rule the status pills and compliance rows follow.
        row->setText(3, QStringLiteral("%1 %2").arg(
                             glyph_for(adapter.link),
                             QString::fromStdString(core::to_string(adapter.link))));

        const QColor colour = colour_for(adapter.link);
        const QString tooltip = tr("%1\n%2\n\nType:\t%3\nLink:\t%4\nId:\t%5")
                                    .arg(QString::fromStdString(adapter.name),
                                          QString::fromStdString(adapter.description),
                                          QString::fromStdString(core::to_string(adapter.type)),
                                          explain(adapter.link),
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
