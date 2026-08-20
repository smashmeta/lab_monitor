#pragma once

#include "lm/ui/export.hpp"

#include <QStyledItemDelegate>

namespace lm::ui {

/// Keeps an item's own Qt::ForegroundRole colour when its row is selected.
///
/// By default a selected item is painted with QPalette::HighlightedText, which
/// throws away whatever the model returned for Qt::ForegroundRole -- so a fleet
/// row colour-coded by health turned plain white the moment it was clicked,
/// losing exactly the information the colour was carrying. This overrides
/// HighlightedText per item with the model's own colour, leaving the selection
/// *background* to the style so the row is still obviously selected.
class LM_UI_EXPORT KeepForegroundDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override;
};

}  // namespace lm::ui
