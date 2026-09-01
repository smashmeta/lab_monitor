#include "add_rule_dialog.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "lm/ui/rule_detail.hpp"
#include "lm/ui/theme.hpp"

namespace {

// The pages, in the order kind_choices() lists them. Named rather than bare
// indices so the stack and the combo cannot drift apart unnoticed.
enum Page { kProcess = 0, kService, kRegistry, kAdapterCount, kAdapterState, kDdsTopic, kDdsValue };

const QStringList& link_state_choices() {
    // Unknown is deliberately absent: "this adapter must be in a state the
    // client could not determine" is not a check anyone means to write.
    static const QStringList choices{
        QStringLiteral("Up"),         QStringLiteral("No link"),  QStringLiteral("Disconnected"),
        QStringLiteral("Connecting"), QStringLiteral("Disabled"), QStringLiteral("Faulted")};
    return choices;
}

lm::core::LinkState link_state_from_choice(const QString& text) {
    if (text == QStringLiteral("Up"))         return lm::core::LinkState::Connected;
    if (text == QStringLiteral("No link"))    return lm::core::LinkState::NoMedia;
    if (text == QStringLiteral("Connecting")) return lm::core::LinkState::Connecting;
    if (text == QStringLiteral("Disabled"))   return lm::core::LinkState::Disabled;
    if (text == QStringLiteral("Faulted"))    return lm::core::LinkState::Faulted;
    return lm::core::LinkState::Disconnected;
}

lm::core::DdsMatch dds_match_from_choice(const QString& text) {
    if (text == QStringLiteral("containing")) return lm::core::DdsMatch::Contains;
    if (text == QStringLiteral("at least"))   return lm::core::DdsMatch::AtLeast;
    if (text == QStringLiteral("at most"))    return lm::core::DdsMatch::AtMost;
    return lm::core::DdsMatch::Equals;
}

QLineEdit* line_edit(const char* object_name, const QString& placeholder) {
    auto* edit = new QLineEdit();
    edit->setObjectName(QString::fromLatin1(object_name));
    // Every text field carries an example. The reported mis-entry -- a path
    // typed into the topic box -- is exactly what a placeholder showing
    // "ShoppingCart" beside one showing "items_.length" prevents.
    edit->setPlaceholderText(placeholder);
    return edit;
}

QSpinBox* domain_spin(const char* object_name) {
    auto* spin = new QSpinBox();
    spin->setObjectName(QString::fromLatin1(object_name));
    spin->setRange(0, 232);  // the DDS domain range
    spin->setValue(0);
    return spin;
}

QComboBox* combo(const char* object_name, const QStringList& items) {
    auto* box = new QComboBox();
    box->setObjectName(QString::fromLatin1(object_name));
    box->addItems(items);
    return box;
}

}  // namespace

QStringList AddRuleDialog::kind_choices() {
    return {QStringLiteral("Process"),
            QStringLiteral("Service"),
            QStringLiteral("Registry"),
            QStringLiteral("Network: adapter count"),
            QStringLiteral("Network: named adapter"),
            QStringLiteral("DDS: topic is published"),
            QStringLiteral("DDS: value on a topic")};
}

AddRuleDialog::AddRuleDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Add Rule"));
    setObjectName(QStringLiteral("AddRuleDialog"));

    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    kind_box_ = combo("KindBox", kind_choices());
    form->addRow(QStringLiteral("Kind:"), kind_box_);

    description_edit_ = line_edit("DescriptionEdit", QStringLiteral("optional, shown on the wall display"));
    form->addRow(QStringLiteral("Description:"), description_edit_);

    expectation_box_ = combo("ExpectationBox",
                              {QStringLiteral("Must be present"), QStringLiteral("Must be absent")});
    expectation_label_ = new QLabel(QStringLiteral("Expectation:"), this);
    form->addRow(expectation_label_, expectation_box_);
    layout->addLayout(form);

    // One page per kind, so the form never shows a box this rule has no use
    // for -- the whole point of replacing the prompt chain.
    pages_ = new QStackedWidget(this);
    pages_->setObjectName(QStringLiteral("KindPages"));
    pages_->addWidget(build_process_page());
    pages_->addWidget(build_service_page());
    pages_->addWidget(build_registry_page());
    pages_->addWidget(build_adapter_count_page());
    pages_->addWidget(build_adapter_state_page());
    pages_->addWidget(build_dds_topic_page());
    pages_->addWidget(build_dds_value_page());
    layout->addWidget(pages_);

    auto* rule_line = new QFrame(this);
    rule_line->setFrameShape(QFrame::HLine);
    layout->addWidget(rule_line);

    // The line the reported mistake needed. It says, in the same words the rule
    // table uses, what this will actually check -- so a path in the topic box
    // reads back as "items_.length.items_.length" before anything is saved.
    summary_label_ = new QLabel(this);
    summary_label_->setObjectName(QStringLiteral("SummaryLabel"));
    summary_label_->setWordWrap(true);
    layout->addWidget(summary_label_);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons_->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Add"));
    layout->addWidget(buttons_);

    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(kind_box_, &QComboBox::currentTextChanged, this, &AddRuleDialog::on_kind_changed);

    // Everything that can change the rule refreshes the summary and the Add
    // button. Connected in one sweep rather than one by one: a field added
    // later and forgotten here would leave the summary quietly lying.
    for (QLineEdit* edit : findChildren<QLineEdit*>()) {
        connect(edit, &QLineEdit::textChanged, this, &AddRuleDialog::refresh);
    }
    for (QComboBox* box : findChildren<QComboBox*>()) {
        connect(box, &QComboBox::currentTextChanged, this, &AddRuleDialog::refresh);
    }
    for (QSpinBox* spin : findChildren<QSpinBox*>()) {
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, &AddRuleDialog::refresh);
    }

    on_kind_changed();
    resize(520, sizeHint().height());
}

QWidget* AddRuleDialog::build_process_page() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    process_executable_ = line_edit("ProcessExecutable", QStringLiteral("antivirus.exe"));
    form->addRow(QStringLiteral("Executable:"), process_executable_);
    return page;
}

QWidget* AddRuleDialog::build_service_page() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    service_name_ = line_edit("ServiceName", QStringLiteral("Spooler"));
    form->addRow(QStringLiteral("Service name:"), service_name_);
    return page;
}

QWidget* AddRuleDialog::build_registry_page() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    registry_hive_ = combo("RegistryHive", {QStringLiteral("HKLM"), QStringLiteral("HKCU"),
                                             QStringLiteral("HKCR"), QStringLiteral("HKU")});
    registry_key_path_ = line_edit("RegistryKeyPath", QStringLiteral("SOFTWARE\\Acme"));
    registry_value_name_ = line_edit("RegistryValueName", QStringLiteral("Version"));
    registry_match_ = combo("RegistryMatch", {QStringLiteral("Exists"), QStringLiteral("Equals"),
                                               QStringLiteral("Contains")});
    registry_expected_ = line_edit("RegistryExpected", QStringLiteral("2.0"));
    form->addRow(QStringLiteral("Hive:"), registry_hive_);
    form->addRow(QStringLiteral("Key path:"), registry_key_path_);
    form->addRow(QStringLiteral("Value name:"), registry_value_name_);
    form->addRow(QStringLiteral("Match:"), registry_match_);
    form->addRow(QStringLiteral("Expected value:"), registry_expected_);
    return page;
}

QWidget* AddRuleDialog::build_adapter_count_page() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    adapter_comparison_ = combo("AdapterComparison", {QStringLiteral("at least"),
                                                       QStringLiteral("exactly"),
                                                       QStringLiteral("at most")});
    adapter_count_ = new QSpinBox(page);
    adapter_count_->setObjectName(QStringLiteral("AdapterCount"));
    adapter_count_->setRange(0, 64);
    adapter_count_->setValue(1);
    form->addRow(QStringLiteral("Connected adapters:"), adapter_comparison_);
    form->addRow(QStringLiteral("Count:"), adapter_count_);
    return page;
}

QWidget* AddRuleDialog::build_adapter_state_page() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    // Free text rather than a picker: the server does not know which adapters a
    // host has until that host reports, and rules are routinely written for
    // machines that have not checked in yet.
    adapter_name_ = line_edit("AdapterName", QStringLiteral("smash-wifi"));
    adapter_link_ = combo("AdapterLink", link_state_choices());
    form->addRow(QStringLiteral("Adapter name:"), adapter_name_);
    form->addRow(QStringLiteral("Required link:"), adapter_link_);
    return page;
}

QWidget* AddRuleDialog::build_dds_topic_page() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    dds_topic_domain_ = domain_spin("DdsTopicDomain");
    dds_topic_name_ = line_edit("DdsTopicName", QStringLiteral("ShoppingCart"));
    form->addRow(QStringLiteral("Domain id:"), dds_topic_domain_);
    form->addRow(QStringLiteral("Topic name:"), dds_topic_name_);
    return page;
}

QWidget* AddRuleDialog::build_dds_value_page() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    dds_value_domain_ = domain_spin("DdsValueDomain");
    dds_value_topic_ = line_edit("DdsValueTopic", QStringLiteral("ShoppingCart"));
    dds_value_path_ = line_edit("DdsValuePath", QStringLiteral("items_.length"));
    dds_value_match_ = combo("DdsValueMatch", {QStringLiteral("equal to"),
                                                QStringLiteral("containing"),
                                                QStringLiteral("at least"),
                                                QStringLiteral("at most")});
    dds_value_expected_ = line_edit("DdsValueExpected", QStringLiteral("2"));

    form->addRow(QStringLiteral("Domain id:"), dds_value_domain_);
    form->addRow(QStringLiteral("Topic name:"), dds_value_topic_);
    form->addRow(QStringLiteral("Path in the sample:"), dds_value_path_);
    form->addRow(QStringLiteral("Value must be:"), dds_value_match_);
    form->addRow(QStringLiteral("Expected:"), dds_value_expected_);

    // The server has never seen this type and cannot offer its fields, so the
    // grammar has to be written down where the path is typed.
    auto* hint = new QLabel(
        QStringLiteral("Path addresses a value: <code>status</code>, "
                        "<code>items_.length</code>, <code>items_[0].sku</code>.<br>"
                        "<code>[*]</code> addresses every element and the rule passes if "
                        "<b>any</b> of them matches: <code>items_[*].sku</code>"),
        page);
    hint->setObjectName(QStringLiteral("DdsPathHint"));
    hint->setTextFormat(Qt::RichText);
    hint->setWordWrap(true);
    form->addRow(QString(), hint);
    return page;
}

bool AddRuleDialog::kind_uses_expectation() const {
    const int page = pages_->currentIndex();
    return page != kAdapterCount && page != kDdsValue;
}

void AddRuleDialog::on_kind_changed() {
    pages_->setCurrentIndex(kind_box_->currentIndex());
    const bool asks = kind_uses_expectation();
    expectation_label_->setVisible(asks);
    expectation_box_->setVisible(asks);
    refresh();
}

lm::core::Rule AddRuleDialog::rule() const {
    lm::core::Rule rule;
    rule.description = description_edit_->text().trimmed().toStdString();
    rule.expectation = (kind_uses_expectation() &&
                        expectation_box_->currentText() == QStringLiteral("Must be absent"))
                            ? lm::core::Presence::MustBeAbsent
                            : lm::core::Presence::MustBePresent;

    switch (pages_->currentIndex()) {
        case kProcess:
            rule.payload = lm::core::ProcessRule{process_executable_->text().trimmed().toStdString()};
            break;
        case kService:
            rule.payload =
                lm::core::ServiceRule{service_name_->text().trimmed().toStdString(), std::nullopt};
            break;
        case kRegistry: {
            lm::core::RegistryRule payload;
            payload.hive = lm::core::parse_registry_hive(registry_hive_->currentText().toStdString())
                               .value_or(lm::core::RegistryHive::LocalMachine);
            payload.key_path = registry_key_path_->text().trimmed().toStdString();
            payload.value_name = registry_value_name_->text().trimmed().toStdString();
            payload.match = registry_match_->currentText() == QStringLiteral("Equals")
                                ? lm::core::RegistryMatch::Equals
                            : registry_match_->currentText() == QStringLiteral("Contains")
                                ? lm::core::RegistryMatch::Contains
                                : lm::core::RegistryMatch::Exists;
            payload.expected_value = registry_expected_->text().trimmed().toStdString();
            rule.payload = payload;
            break;
        }
        case kAdapterCount: {
            lm::core::AdapterCountRule payload;
            payload.comparison = adapter_comparison_->currentText() == QStringLiteral("exactly")
                                     ? lm::core::Comparison::Exactly
                                 : adapter_comparison_->currentText() == QStringLiteral("at most")
                                     ? lm::core::Comparison::AtMost
                                     : lm::core::Comparison::AtLeast;
            payload.count = adapter_count_->value();
            rule.payload = payload;
            break;
        }
        case kAdapterState:
            rule.payload = lm::core::AdapterStateRule{
                adapter_name_->text().trimmed().toStdString(),
                link_state_from_choice(adapter_link_->currentText())};
            break;
        case kDdsTopic:
            rule.payload =
                lm::core::DdsTopicRule{static_cast<std::uint32_t>(dds_topic_domain_->value()),
                                        dds_topic_name_->text().trimmed().toStdString()};
            break;
        default: {
            lm::core::DdsValueRule payload;
            payload.domain_id = static_cast<std::uint32_t>(dds_value_domain_->value());
            payload.topic_name = dds_value_topic_->text().trimmed().toStdString();
            payload.path = dds_value_path_->text().trimmed().toStdString();
            payload.match = dds_match_from_choice(dds_value_match_->currentText());
            payload.expected_value = dds_value_expected_->text().trimmed().toStdString();
            rule.payload = payload;
            break;
        }
    }
    return rule;
}

bool AddRuleDialog::is_complete() const {
    const auto filled = [](const QLineEdit* edit) { return !edit->text().trimmed().isEmpty(); };

    switch (pages_->currentIndex()) {
        case kProcess:      return filled(process_executable_);
        case kService:      return filled(service_name_);
        case kRegistry:
            // A value name is not required: a rule may assert that a key
            // exists, which is what registry_key() renders with an empty leaf.
            return filled(registry_key_path_) &&
                   (registry_match_->currentText() == QStringLiteral("Exists") ||
                    filled(registry_expected_));
        case kAdapterCount: return true;  // a comparison and a number, both always set
        case kAdapterState: return filled(adapter_name_);
        case kDdsTopic:     return filled(dds_topic_name_);
        default:            return filled(dds_value_topic_) && filled(dds_value_path_) &&
                                    filled(dds_value_expected_);
    }
}

QString AddRuleDialog::summary() const {
    if (!is_complete()) {
        return QStringLiteral("Fill in the fields above to see what this rule will check.");
    }
    // describe() is what the rule table and both detail panes use, so the
    // sentence here is the one the operator will read back afterwards.
    //
    // The kind is deliberately not repeated: it is chosen in the combo two
    // inches above, and lowercasing it for the sentence turned "DDS" into
    // "dds".
    const lm::ui::RuleDetail detail = lm::ui::describe(rule());
    return QStringLiteral("Checks %1 — %2").arg(detail.target, detail.expectation);
}

void AddRuleDialog::refresh() {
    const bool complete = is_complete();
    summary_label_->setText(summary());
    summary_label_->setStyleSheet(
        QStringLiteral("color: %1;")
            .arg(complete ? lm::ui::Theme::kText : lm::ui::Theme::kTextMuted));
    // Disabled rather than validated on Add: an incomplete rule should never
    // look like something that can be created. The prompt chain this replaced
    // instead threw the whole flow away on the first empty answer.
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(complete);
}
