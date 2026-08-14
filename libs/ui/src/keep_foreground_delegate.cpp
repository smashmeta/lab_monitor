#include "lm/ui/keep_foreground_delegate.hpp"

#include <QColor>
#include <QStyleOptionViewItem>

namespace lm::ui {

void KeepForegroundDelegate::initStyleOption(QStyleOptionViewItem* option,
                                             const QModelIndex& index) const {
    QStyledItemDelegate::initStyleOption(option, index);

    const QVariant foreground = index.data(Qt::ForegroundRole);
    if (!foreground.isValid()) {
        return;
    }

    const QColor colour = foreground.value<QColor>();
    if (!colour.isValid()) {
        return;
    }

    // Text covers the unselected case (which the base class already handled),
    // HighlightedText the selected one -- that second assignment is the whole
    // point of this class.
    option->palette.setColor(QPalette::Text, colour);
    option->palette.setColor(QPalette::HighlightedText, colour);
}

}  // namespace lm::ui
