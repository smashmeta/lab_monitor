#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

#include <string>
#include <variant>

#include "add_rule_dialog.hpp"

using namespace lm::core;

namespace {

/// Every field carries an object name, which is what makes this dialog
/// testable at all — the chain of QInputDialog prompts it replaced could only
/// be driven by a human.
void type(AddRuleDialog& dialog, const char* name, const QString& text) {
    auto* edit = dialog.findChild<QLineEdit*>(QString::fromLatin1(name));
    ASSERT_NE(edit, nullptr) << "no field named " << name;
    edit->setText(text);
}

void choose(AddRuleDialog& dialog, const char* name, const QString& text) {
    auto* box = dialog.findChild<QComboBox*>(QString::fromLatin1(name));
    ASSERT_NE(box, nullptr) << "no combo named " << name;
    box->setCurrentText(text);
    ASSERT_EQ(box->currentText(), text) << name << " has no option " << text.toStdString();
}

void spin(AddRuleDialog& dialog, const char* name, int value) {
    auto* box = dialog.findChild<QSpinBox*>(QString::fromLatin1(name));
    ASSERT_NE(box, nullptr) << "no spin box named " << name;
    box->setValue(value);
}

bool add_enabled(AddRuleDialog& dialog) {
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    return buttons != nullptr && buttons->button(QDialogButtonBox::Ok)->isEnabled();
}

}  // namespace

TEST(AddRuleDialog, OffersEveryKindTheStackSupports) {
    // A kind missing here is a rule nobody can create, however completely the
    // rest of the stack supports it.
    const QStringList kinds = AddRuleDialog::kind_choices();
    for (const char* expected : {"Process", "Service", "Registry", "Network: adapter count",
                                  "Network: named adapter", "DDS: topic is published",
                                  "DDS: value on a topic"}) {
        EXPECT_TRUE(kinds.contains(QString::fromLatin1(expected)))
            << expected << " missing from " << kinds.join(", ").toStdString();
    }
}

TEST(AddRuleDialog, BuildsAProcessRule) {
    AddRuleDialog dialog;
    choose(dialog, "KindBox", QStringLiteral("Process"));
    type(dialog, "ProcessExecutable", QStringLiteral("antivirus.exe"));
    choose(dialog, "ExpectationBox", QStringLiteral("Must be absent"));

    const Rule rule = dialog.rule();
    ASSERT_TRUE(std::holds_alternative<ProcessRule>(rule.payload));
    EXPECT_EQ(std::get<ProcessRule>(rule.payload).executable, "antivirus.exe");
    EXPECT_EQ(rule.expectation, Presence::MustBeAbsent);
}

TEST(AddRuleDialog, BuildsARegistryRule) {
    AddRuleDialog dialog;
    choose(dialog, "KindBox", QStringLiteral("Registry"));
    choose(dialog, "RegistryHive", QStringLiteral("HKCU"));
    type(dialog, "RegistryKeyPath", QStringLiteral("SOFTWARE\\Acme"));
    type(dialog, "RegistryValueName", QStringLiteral("Version"));
    choose(dialog, "RegistryMatch", QStringLiteral("Equals"));
    type(dialog, "RegistryExpected", QStringLiteral("2.0"));

    const Rule rule = dialog.rule();
    ASSERT_TRUE(std::holds_alternative<RegistryRule>(rule.payload));
    const auto& payload = std::get<RegistryRule>(rule.payload);
    EXPECT_EQ(payload.hive, RegistryHive::CurrentUser);
    EXPECT_EQ(payload.value_name, "Version");
    EXPECT_EQ(payload.match, RegistryMatch::Equals);
    EXPECT_EQ(payload.expected_value, "2.0");
}

TEST(AddRuleDialog, BuildsADdsValueRule) {
    // The motivating case, and the one that was mis-authored through the old
    // prompt chain.
    AddRuleDialog dialog;
    choose(dialog, "KindBox", QStringLiteral("DDS: value on a topic"));
    spin(dialog, "DdsValueDomain", 42);
    type(dialog, "DdsValueTopic", QStringLiteral("ShoppingCart"));
    type(dialog, "DdsValuePath", QStringLiteral("items_.length"));
    choose(dialog, "DdsValueMatch", QStringLiteral("equal to"));
    type(dialog, "DdsValueExpected", QStringLiteral("2"));

    const Rule rule = dialog.rule();
    ASSERT_TRUE(std::holds_alternative<DdsValueRule>(rule.payload));
    const auto& payload = std::get<DdsValueRule>(rule.payload);
    EXPECT_EQ(payload.domain_id, 42u);
    EXPECT_EQ(payload.topic_name, "ShoppingCart");
    EXPECT_EQ(payload.path, "items_.length");
    EXPECT_EQ(payload.match, DdsMatch::Equals);
    EXPECT_EQ(payload.expected_value, "2");
}

TEST(AddRuleDialog, BuildsADdsTopicRule) {
    AddRuleDialog dialog;
    choose(dialog, "KindBox", QStringLiteral("DDS: topic is published"));
    spin(dialog, "DdsTopicDomain", 42);
    type(dialog, "DdsTopicName", QStringLiteral("ShoppingCart"));

    const Rule rule = dialog.rule();
    ASSERT_TRUE(std::holds_alternative<DdsTopicRule>(rule.payload));
    EXPECT_EQ(std::get<DdsTopicRule>(rule.payload).topic_name, "ShoppingCart");
}

TEST(AddRuleDialog, BuildsAnAdapterCountRule) {
    AddRuleDialog dialog;
    choose(dialog, "KindBox", QStringLiteral("Network: adapter count"));
    choose(dialog, "AdapterComparison", QStringLiteral("exactly"));
    spin(dialog, "AdapterCount", 2);

    const Rule rule = dialog.rule();
    ASSERT_TRUE(std::holds_alternative<AdapterCountRule>(rule.payload));
    EXPECT_EQ(std::get<AdapterCountRule>(rule.payload).comparison, Comparison::Exactly);
    EXPECT_EQ(std::get<AdapterCountRule>(rule.payload).count, 2);
}

TEST(AddRuleDialog, HidesTheExpectationForKindsThatCarryTheirOwnDirection) {
    // The adapter count says it in its comparison and the DDS value in its
    // match, so a presence beside either would be a second, contradictable
    // answer. The old chain skipped the prompt; this hides the row.
    AddRuleDialog dialog;
    dialog.show();

    auto* expectation = dialog.findChild<QComboBox*>(QStringLiteral("ExpectationBox"));
    ASSERT_NE(expectation, nullptr);

    choose(dialog, "KindBox", QStringLiteral("Process"));
    EXPECT_TRUE(expectation->isVisible());

    choose(dialog, "KindBox", QStringLiteral("Network: adapter count"));
    EXPECT_FALSE(expectation->isVisible());

    choose(dialog, "KindBox", QStringLiteral("DDS: value on a topic"));
    EXPECT_FALSE(expectation->isVisible());
}

TEST(AddRuleDialog, ShowsOnlyTheFieldsForTheChosenKind) {
    AddRuleDialog dialog;
    dialog.show();

    choose(dialog, "KindBox", QStringLiteral("Process"));
    EXPECT_TRUE(dialog.findChild<QLineEdit*>(QStringLiteral("ProcessExecutable"))->isVisible());
    EXPECT_FALSE(dialog.findChild<QLineEdit*>(QStringLiteral("DdsValuePath"))->isVisible());

    choose(dialog, "KindBox", QStringLiteral("DDS: value on a topic"));
    EXPECT_TRUE(dialog.findChild<QLineEdit*>(QStringLiteral("DdsValuePath"))->isVisible());
    EXPECT_FALSE(dialog.findChild<QLineEdit*>(QStringLiteral("ProcessExecutable"))->isVisible());
}

TEST(AddRuleDialog, RefusesToAddUntilTheRequiredFieldsAreFilled) {
    // The chain abandoned the whole flow on the first empty answer. Here an
    // incomplete rule simply cannot be created, and the button says so.
    AddRuleDialog dialog;
    choose(dialog, "KindBox", QStringLiteral("DDS: value on a topic"));
    EXPECT_FALSE(add_enabled(dialog));

    type(dialog, "DdsValueTopic", QStringLiteral("ShoppingCart"));
    EXPECT_FALSE(add_enabled(dialog)) << "still needs a path and an expected value";
    type(dialog, "DdsValuePath", QStringLiteral("items_.length"));
    EXPECT_FALSE(add_enabled(dialog)) << "still needs an expected value";
    type(dialog, "DdsValueExpected", QStringLiteral("2"));
    EXPECT_TRUE(add_enabled(dialog));
}

TEST(AddRuleDialog, ARegistryExistsRuleNeedsNoExpectedValue) {
    AddRuleDialog dialog;
    choose(dialog, "KindBox", QStringLiteral("Registry"));
    type(dialog, "RegistryKeyPath", QStringLiteral("SOFTWARE\\Acme"));
    choose(dialog, "RegistryMatch", QStringLiteral("Exists"));
    EXPECT_TRUE(add_enabled(dialog));

    choose(dialog, "RegistryMatch", QStringLiteral("Equals"));
    EXPECT_FALSE(add_enabled(dialog)) << "Equals against nothing is not a check";
}

TEST(AddRuleDialog, SaysWhatTheRuleWillCheckBeforeItIsCreated) {
    // The line the reported mistake needed. Typing the path into the topic box
    // reads straight back as a target nobody meant to write.
    AddRuleDialog dialog;
    choose(dialog, "KindBox", QStringLiteral("DDS: value on a topic"));
    spin(dialog, "DdsValueDomain", 42);
    type(dialog, "DdsValueTopic", QStringLiteral("items_.length"));  // the mis-entry
    type(dialog, "DdsValuePath", QStringLiteral("items_.length"));
    type(dialog, "DdsValueExpected", QStringLiteral("2"));

    EXPECT_NE(dialog.summary().indexOf(QStringLiteral("items_.length.items_.length")), -1)
        << "the summary must show the mistake: " << dialog.summary().toStdString();

    // ...and reads correctly once the topic is the topic.
    type(dialog, "DdsValueTopic", QStringLiteral("ShoppingCart"));
    EXPECT_NE(dialog.summary().indexOf(QStringLiteral("ShoppingCart.items_.length on domain 42")), -1)
        << dialog.summary().toStdString();
}

TEST(AddRuleDialog, SaysNothingUsefulIsKnownYetWhileIncomplete) {
    AddRuleDialog dialog;
    choose(dialog, "KindBox", QStringLiteral("Process"));
    EXPECT_NE(dialog.summary().indexOf(QStringLiteral("Fill in")), -1)
        << dialog.summary().toStdString();
}

TEST(AddRuleDialog, LeavesTheIdToTheCaller) {
    // Ids are unique across the whole draft, since templates are combined per
    // host -- and this dialog cannot see the draft.
    AddRuleDialog dialog;
    choose(dialog, "KindBox", QStringLiteral("Process"));
    type(dialog, "ProcessExecutable", QStringLiteral("chrome.exe"));
    EXPECT_TRUE(dialog.rule().id.empty());
}
