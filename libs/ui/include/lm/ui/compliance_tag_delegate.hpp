#pragma once

#include "lm/ui/export.hpp"

#include <QSize>
#include <QStyledItemDelegate>

namespace lm::ui {

/// Paints FleetModel::ComplianceColumn: the passed-over-checked ratio, then one
/// rounded tag per rule the host is failing or could not check.
///
/// A delegate rather than widgets in the cell. The column has one of these per
/// row and the fleet table is sized to hold about thirty-five of them, so a
/// QWidget per tag would be hundreds of widgets whose only job is to draw a
/// rectangle and some text.
///
/// The row stays one line high whatever it is failing. Growing the row to fit
/// every tag would cost the table most of its density and leave a column of
/// ragged heights to scan down, so the tags that do not fit are replaced by a
/// muted "+3 more" and the full list lives in the cell's tooltip. That trade is
/// only available because this tab is read by someone sitting at the keyboard.
class LM_UI_EXPORT ComplianceTagDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit ComplianceTagDelegate(QObject* parent = nullptr);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    [[nodiscard]] QSize sizeHint(const QStyleOptionViewItem& option,
                                 const QModelIndex& index) const override;
};

}  // namespace lm::ui
