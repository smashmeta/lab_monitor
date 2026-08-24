#include "lm/ui/token_edit.hpp"

#include <QAbstractItemView>
#include <QCompleter>
#include <QEvent>
#include <QFocusEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QStringListModel>
#include <QStyle>
#include <QToolButton>

#include <algorithm>
#include <utility>

namespace lm::ui {

TokenEdit::TokenEdit(QWidget* parent)
    : QWidget(parent),
      layout_(new QHBoxLayout(this)),
      input_(new QLineEdit(this)),
      completer_(new QCompleter(this)) {
    setObjectName(QStringLiteral("TokenEdit"));
    // The container draws the field's frame, so the input inside it must not
    // draw a second one -- chips and text have to read as one field.
    setFocusProxy(input_);

    layout_->setContentsMargins(4, 2, 4, 2);
    layout_->setSpacing(4);

    input_->setObjectName(QStringLiteral("TokenEditInput"));
    input_->setFrame(false);
    input_->setProperty("unknown", false);
    input_->installEventFilter(this);
    layout_->addWidget(input_, 1);

    completions_ = new QStringListModel(completer_);
    completer_->setModel(completions_);
    completer_->setCaseSensitivity(Qt::CaseInsensitive);
    completer_->setCompletionMode(QCompleter::PopupCompletion);
    // Contains rather than prefix: "server" should find "Build Server", since
    // the operator is picking from a list they only half remember.
    completer_->setFilterMode(Qt::MatchContains);
    // setWidget(), deliberately *not* input_->setCompleter().
    //
    // QLineEdit::setCompleter() wires the completer's activated() to the line
    // edit's own setText(), which makes QLineEdit a second writer to a field
    // this class is supposed to own. Accepting a completion then became a race
    // between two connections: ours clears the input, QLineEdit's puts the
    // completion back, and whichever runs last wins. When QLineEdit won, the
    // committed name stayed in the field beside the chip that had just been
    // made from it -- and vanished on focus-out, because the focus-out commit
    // clears with nobody left to write it back. That is the leftover text.
    //
    // setWidget() gives the popup, its filtering and its key handling with
    // none of that plumbing, so the prefix below and the handler further down
    // are the only things that ever write here.
    completer_->setWidget(input_);

    connect(input_, &QLineEdit::returnPressed, this, &TokenEdit::commit_input);
    connect(input_, &QLineEdit::textChanged, this, &TokenEdit::refresh_unknown_hint);

    // textEdited, not textChanged: only a comma the *user* produced separates
    // tokens, and only text the user typed should reopen the popup. Either one
    // we put back ourselves would recurse.
    //
    // Driving the completer from here is the cost of owning the field, and it
    // fixes a second-order bug on the way: with setCompleter() the prefix
    // outlived the text it came from, so clearing the input left the completer
    // still holding the last thing typed for any later model change to act on.
    connect(input_, &QLineEdit::textEdited, this, [this](const QString& text) {
        if (!text.contains(QChar(u','))) {
            update_completions(text);
            return;
        }
        QStringList parts = text.split(QChar(u','));
        const QString remainder = parts.takeLast();
        bool added = false;
        for (const QString& part : parts) {
            added = add_token(part) || added;
        }
        input_->setText(remainder);
        update_completions(remainder);
        if (added) {
            emit tokens_changed(tokens_);
        }
    });

    // The only writer besides commit_input(), now that QLineEdit is not one.
    // No ordering assumption left to be wrong about.
    connect(completer_, qOverload<const QString&>(&QCompleter::activated), this,
            [this](const QString& value) {
                clear_input();
                if (add_token(value)) {
                    emit tokens_changed(tokens_);
                }
            });
}

void TokenEdit::clear_input() {
    input_->clear();
    // The completer's prefix has to go with the text. Left behind, it is a
    // pending completion for something no longer being typed, which a later
    // model change can act on.
    completer_->setCompletionPrefix(QString());
    if (completer_->popup() != nullptr) {
        completer_->popup()->hide();
    }
}

void TokenEdit::update_completions(const QString& text) {
    completer_->setCompletionPrefix(text);
    if (text.trimmed().isEmpty() || completer_->completionCount() == 0) {
        if (completer_->popup() != nullptr) {
            completer_->popup()->hide();
        }
        return;
    }
    completer_->complete();
}

void TokenEdit::set_known_values(QStringList values) {
    known_ = std::move(values);
    completions_->setStringList(known_);
    // A value can become known while it is still being typed -- the caller
    // creating it in response to an earlier commit, say -- and the hint has to
    // stop marking it.
    refresh_unknown_hint();
}

void TokenEdit::set_tokens(QStringList tokens) {
    tokens_ = std::move(tokens);
    rebuild_chips();
}

void TokenEdit::commit_input() {
    const QString text = input_->text();
    clear_input();
    if (add_token(text)) {
        emit tokens_changed(tokens_);
    }
}

bool TokenEdit::add_token(const QString& value) {
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty() || holds(trimmed)) {
        return false;
    }
    tokens_.push_back(trimmed);
    rebuild_chips();
    return true;
}

void TokenEdit::remove_token(const QString& value) {
    const int before = static_cast<int>(tokens_.size());
    tokens_.removeAll(value);
    if (static_cast<int>(tokens_.size()) == before) {
        return;
    }
    rebuild_chips();
    emit tokens_changed(tokens_);
}

void TokenEdit::rebuild_chips() {
    for (QWidget* chip : chips_) {
        // Reparented before deleteLater() so it is out of findChild() and off
        // the layout immediately: the deletion itself cannot happen now, since
        // this often runs from inside a chip button's own clicked handler.
        chip->setParent(nullptr);
        chip->deleteLater();
    }
    chips_.clear();

    int position = 0;
    for (const QString& token : tokens_) {
        auto* chip = new QFrame(this);
        chip->setObjectName(QStringLiteral("TokenChip"));

        auto* chip_layout = new QHBoxLayout(chip);
        chip_layout->setContentsMargins(8, 1, 3, 1);
        chip_layout->setSpacing(4);
        chip_layout->addWidget(new QLabel(token, chip));

        auto* remove = new QToolButton(chip);
        remove->setObjectName(QStringLiteral("TokenChipRemove"));
        remove->setText(QStringLiteral("×"));  // multiplication sign, not a letter x
        remove->setToolTip(tr("Remove %1").arg(token));
        remove->setProperty("token", token);
        connect(remove, &QToolButton::clicked, this, [this, token] { remove_token(token); });
        chip_layout->addWidget(remove);

        layout_->insertWidget(position, chip);
        chips_.push_back(chip);
        ++position;
    }

    // Settles the geometry now rather than on a later pass of the event loop,
    // so anything reading sizeHint() straight after (the assignment table sizes
    // its rows from it) sees the chips that were just added.
    layout_->activate();
}

void TokenEdit::refresh_unknown_hint() {
    const QString text = input_->text().trimmed();
    const bool unknown = !text.isEmpty() && !is_known(text);
    if (input_->property("unknown").toBool() == unknown) {
        return;
    }
    input_->setProperty("unknown", unknown);
    // A dynamic property reaches the stylesheet only when the style is asked to
    // look at the widget again.
    input_->style()->unpolish(input_);
    input_->style()->polish(input_);
}

bool TokenEdit::is_known(const QString& value) const {
    return std::ranges::any_of(
        known_, [&](const QString& known) { return known.compare(value, Qt::CaseInsensitive) == 0; });
}

bool TokenEdit::holds(const QString& value) const {
    return std::ranges::any_of(
        tokens_, [&](const QString& token) { return token.compare(value, Qt::CaseInsensitive) == 0; });
}

bool TokenEdit::eventFilter(QObject* watched, QEvent* event) {
    if (watched != input_) {
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Backspace && input_->text().isEmpty() && !tokens_.isEmpty()) {
            remove_token(tokens_.back());
            return true;
        }
    } else if (event->type() == QEvent::FocusOut) {
        const auto* focus = static_cast<QFocusEvent*>(event);
        // Not when a popup took the focus: that is the completer's own dropdown
        // opening, and committing the half-typed text under it would throw away
        // the completion the user is in the middle of choosing.
        if (focus->reason() != Qt::PopupFocusReason) {
            commit_input();
        }
    }

    return QWidget::eventFilter(watched, event);
}

}  // namespace lm::ui
