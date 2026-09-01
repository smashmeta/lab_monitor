#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

#include "lm/core/rule.hpp"

class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QStackedWidget;
class QWidget;

/// Everything a rule needs, on one surface.
///
/// This replaced a chain of seven `QInputDialog` prompts. The chain worked, but
/// it showed one question at a time with no way to see what was being built
/// until the rule was already in the table — and a DDS rule was duly authored
/// with the path typed into the topic box, producing
/// `items_.length.items_.length on domain 42` and no hint that anything was
/// wrong. A single form makes the fields visible beside each other, and the
/// summary line at the bottom says in words what the rule will check, updated
/// on every keystroke.
///
/// Choosing a kind reveals only that kind's fields, so the form is never
/// showing a box the current rule has no use for.
class AddRuleDialog : public QDialog {
    Q_OBJECT

public:
    explicit AddRuleDialog(QWidget* parent = nullptr);

    /// The kinds offered, in the order they are offered. Public and static so a
    /// test can assert the list without driving the widget: a kind missing here
    /// is a rule nobody can create, however completely the stack supports it.
    [[nodiscard]] static QStringList kind_choices();

    /// Loads an existing rule for editing: fills every field, retitles the
    /// dialog, relabels Add as Save, and locks the kind.
    ///
    /// The kind is locked because changing it keeps nothing of the original
    /// rule but its position in the list -- that is a Remove plus an Add, and
    /// pretending otherwise would let a generated id like process-chrome-exe
    /// end up naming a registry rule.
    void set_rule(const lm::core::Rule& rule);

    /// Whether set_rule() was called. The caller keeps the rule's existing id
    /// on save rather than regenerating it, so that reports already received
    /// under that id keep their labels through the edit.
    [[nodiscard]] bool is_editing() const { return editing_; }

    /// The configured rule, with **no id** — the caller generates that from the
    /// bundle, because uniqueness is bundle-wide and this dialog cannot see it.
    /// Only meaningful once accepted; call is_complete() otherwise.
    [[nodiscard]] lm::core::Rule rule() const;

    /// Whether the required fields for the chosen kind are filled. Drives the
    /// Add button, so an incomplete rule cannot be created at all — the chain
    /// this replaced instead abandoned the whole flow on the first empty answer.
    [[nodiscard]] bool is_complete() const;

    /// One sentence describing what the rule will check, the same phrasing the
    /// rule table uses. Exposed for tests; on screen it is the summary label.
    [[nodiscard]] QString summary() const;

private slots:
    void on_kind_changed();
    void refresh();

private:
    QWidget* build_process_page();
    QWidget* build_service_page();
    QWidget* build_registry_page();
    QWidget* build_adapter_count_page();
    QWidget* build_adapter_state_page();
    QWidget* build_dds_topic_page();
    QWidget* build_dds_value_page();

    /// True for the kinds whose own field carries the direction — the adapter
    /// count in its comparison, the DDS value in its match. Asking those for a
    /// presence as well would be a second, contradictable answer.
    [[nodiscard]] bool kind_uses_expectation() const;

    bool editing_ = false;

    QComboBox* kind_box_;
    QLineEdit* description_edit_;
    QComboBox* expectation_box_;
    QLabel* expectation_label_;
    QStackedWidget* pages_;
    QLabel* summary_label_;
    QDialogButtonBox* buttons_;

    QLineEdit* process_executable_;
    QLineEdit* service_name_;
    QComboBox* registry_hive_;
    QLineEdit* registry_key_path_;
    QLineEdit* registry_value_name_;
    QComboBox* registry_match_;
    QLineEdit* registry_expected_;
    QComboBox* adapter_comparison_;
    QSpinBox* adapter_count_;
    QLineEdit* adapter_name_;
    QComboBox* adapter_link_;
    QSpinBox* dds_topic_domain_;
    QLineEdit* dds_topic_name_;
    QSpinBox* dds_value_domain_;
    QLineEdit* dds_value_topic_;
    QLineEdit* dds_value_path_;
    QComboBox* dds_value_match_;
    QLineEdit* dds_value_expected_;
};
