#pragma once

#include <QStringList>
#include <QVector>
#include <QWidget>

class QCompleter;
class QHBoxLayout;
class QLineEdit;
class QStringListModel;

namespace lm::ui {

/// An Outlook-style recipient field: committed values become chips carrying
/// their own remove button, and a completer offers the known values while the
/// user types.
///
/// The widget takes no view on what an unknown value means. It completes
/// against the known list, marks the input while the text matches nothing in
/// it, and reports the committed tokens — whether an unknown value is an error
/// or something to create on the spot is the caller's policy, expressed by
/// reacting to tokens_changed(). That is what keeps this reusable for anything
/// with a set of names, rather than being a template picker with a general name.
class TokenEdit : public QWidget {
    Q_OBJECT

public:
    explicit TokenEdit(QWidget* parent = nullptr);

    /// The values the completer offers and treats as already existing.
    /// Re-applied to the input's hint, so a value that becomes known while it
    /// is being typed stops being marked.
    void set_known_values(QStringList values);

    [[nodiscard]] QStringList tokens() const { return tokens_; }

    /// Replaces the tokens *without* emitting tokens_changed(): this is the
    /// caller pushing state in, not the user editing, and echoing it back would
    /// loop through whatever slot is listening.
    void set_tokens(QStringList tokens);

signals:
    /// A user edit — a commit, a chip removed, or a backspace. Never emitted
    /// from set_tokens().
    void tokens_changed(QStringList tokens);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void commit_input();
    /// False when the value was rejected as blank or already held.
    bool add_token(const QString& value);
    void remove_token(const QString& value);
    void rebuild_chips();
    void refresh_unknown_hint();
    [[nodiscard]] bool is_known(const QString& value) const;
    [[nodiscard]] bool holds(const QString& value) const;

    QHBoxLayout* layout_ = nullptr;
    QLineEdit* input_ = nullptr;
    QCompleter* completer_ = nullptr;
    QStringListModel* completions_ = nullptr;
    QStringList known_;
    QStringList tokens_;
    QVector<QWidget*> chips_;
};

}  // namespace lm::ui
