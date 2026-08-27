#include "lm/ui/compliance_tag_delegate.hpp"

#include <algorithm>

#include <QApplication>
#include <QFontMetrics>
#include <QPainter>
#include <QRect>
#include <QRectF>
#include <QStyle>
#include <QVector>

#include "lm/ui/fleet_model.hpp"
#include "lm/ui/theme.hpp"

namespace lm::ui {
namespace {

constexpr int kEdgePadding = 6;   ///< cell edge to first thing drawn
constexpr int kGap = 6;           ///< between the ratio and a tag, and between tags
constexpr int kTagPaddingX = 7;   ///< inside a tag, left and right of its text
constexpr int kTagHeight = 17;
constexpr int kTagRadius = 8;
/// A rule description is a sentence, and one long enough to fill the column
/// would push every other tag out. Elided at this width so the *number* of
/// failures stays legible, which is the thing being scanned for.
constexpr int kMaxTagWidth = 170;
/// Fill for a tag's interior. The border and text carry the status colour at
/// full strength; a solid fill at that strength would make a row of tags the
/// loudest thing on a table that also has to show state and load.
constexpr int kTagFillAlpha = 38;

int tag_width(const QFontMetrics& metrics, const QString& label) {
    return std::min(metrics.horizontalAdvance(label) + 2 * kTagPaddingX, kMaxTagWidth);
}

void draw_tag(QPainter& painter, const QFontMetrics& metrics, const QRect& rect,
              const ComplianceTag& tag) {
    // Theme::color_for(), not a colour chosen here: the detail pane below this
    // table paints the same statuses, and two views of one rule disagreeing
    // about its colour is worse than either colour.
    const QColor colour = Theme::color_for(tag.status);
    QColor fill = colour;
    fill.setAlpha(kTagFillAlpha);

    painter.setPen(QPen(colour, 1));
    painter.setBrush(fill);
    // Half-pixel inset so the 1px border lands on the pixel grid rather than
    // straddling it, which antialiasing would otherwise render as two grey rows.
    painter.drawRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), kTagRadius, kTagRadius);

    painter.setPen(colour);
    const QRect text_rect = rect.adjusted(kTagPaddingX, 0, -kTagPaddingX, 0);
    painter.drawText(text_rect, Qt::AlignCenter,
                     metrics.elidedText(tag.label, Qt::ElideRight, text_rect.width()));
}

}  // namespace

ComplianceTagDelegate::ComplianceTagDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

void ComplianceTagDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                  const QModelIndex& index) const {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    const QString ratio = opt.text;
    // Cleared before the style runs, so the style paints the background and the
    // selection and nothing else. With a stylesheet active it is QStyleSheetStyle
    // that draws CE_ItemViewItem, and letting it draw the text too would put the
    // ::item rule's own colour on top of everything decided below.
    opt.text.clear();
    QStyle* style = opt.widget != nullptr ? opt.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    const auto tags = index.data(FleetModel::ComplianceTagsRole).value<QVector<ComplianceTag>>();

    painter->save();
    painter->setClipRect(opt.rect);
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QFontMetrics metrics(opt.font);
    const QRect content = opt.rect.adjusted(kEdgePadding, 0, -kEdgePadding, 0);
    int x = content.left();

    // The ratio is the model's DisplayRole and the row's foreground colour is
    // the model's too -- health, or red for a host that is not reporting.
    const QVariant foreground = index.data(Qt::ForegroundRole);
    const QColor text_colour = foreground.canConvert<QColor>()
                                   ? foreground.value<QColor>()
                                   : opt.palette.color(QPalette::Text);
    if (!ratio.isEmpty()) {
        const int width = metrics.horizontalAdvance(ratio);
        painter->setPen(text_colour);
        painter->drawText(QRect(x, content.top(), width, content.height()),
                          Qt::AlignVCenter | Qt::AlignLeft, ratio);
        x += width + kGap;
    }

    const QColor muted(Theme::kTextMuted);
    const int tag_top = content.center().y() - kTagHeight / 2 + 1;
    int drawn = 0;
    for (const ComplianceTag& tag : tags) {
        const int width = tag_width(metrics, tag.label);
        const int left_after_this = static_cast<int>(tags.size()) - drawn - 1;
        // Room for this tag, and -- if any follow it -- for the marker that will
        // have to say so. Without reserving that, the last tag drawn fills the
        // cell and the "+2 more" it needs has nowhere to go.
        int needed = width;
        if (left_after_this > 0) {
            needed += kGap + metrics.horizontalAdvance(
                                 QStringLiteral("+%1 more").arg(left_after_this));
        }
        if (x + needed > content.right()) {
            break;
        }
        draw_tag(*painter, metrics, QRect(x, tag_top, width, kTagHeight), tag);
        x += width + kGap;
        ++drawn;
    }

    if (drawn < static_cast<int>(tags.size())) {
        painter->setPen(muted);
        painter->drawText(QRect(x, content.top(), content.right() - x, content.height()),
                          Qt::AlignVCenter | Qt::AlignLeft,
                          QStringLiteral("+%1 more").arg(static_cast<int>(tags.size()) - drawn));
    }

    painter->restore();
}

QSize ComplianceTagDelegate::sizeHint(const QStyleOptionViewItem& option,
                                      const QModelIndex& index) const {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(std::max(size.height(), kTagHeight + 6));

    // Width enough for the ratio and the first few tags. Not for *every* tag: a
    // host failing twenty rules would otherwise ask resizeColumnsToContents()
    // for a column wider than the window, and the overflow marker exists
    // precisely so that is not necessary.
    const auto tags = index.data(FleetModel::ComplianceTagsRole).value<QVector<ComplianceTag>>();
    const QFontMetrics metrics(option.font);
    int width = 2 * kEdgePadding + metrics.horizontalAdvance(index.data().toString());
    constexpr int kTagsInHint = 3;
    const int counted = std::min(static_cast<int>(tags.size()), kTagsInHint);
    for (int i = 0; i < counted; ++i) {
        width += kGap + tag_width(metrics, tags[i].label);
    }
    size.setWidth(std::max(size.width(), width));
    return size;
}

}  // namespace lm::ui
