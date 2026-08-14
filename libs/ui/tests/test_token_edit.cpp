#include <gtest/gtest.h>

#include <QApplication>
#include <QCompleter>
#include <QTableWidget>
#include <QLineEdit>
#include <QSignalSpy>
#include <QStringList>
#include <QTest>
#include <QToolButton>

#include <string>

#include "lm/ui/theme.hpp"
#include "lm/ui/token_edit.hpp"
#include "pixel_probe.hpp"

using namespace lm::ui;

namespace {

/// The widget owns its input; tests reach it the way the user's keyboard does,
/// rather than through an accessor that exists only for them.
QLineEdit* input_of(TokenEdit& editor) { return editor.findChild<QLineEdit*>(); }

QToolButton* remove_button_for(TokenEdit& editor, const QString& token) {
    for (QToolButton* button : editor.findChildren<QToolButton*>()) {
        if (button->property("token").toString() == token) {
            return button;
        }
    }
    return nullptr;
}

/// Readable gtest failure output: a raw QStringList prints as a byte dump.
std::string joined(const QStringList& values) {
    return values.join(QStringLiteral(" | ")).toStdString();
}

const QStringList kKnown{QStringLiteral("Lab Workstation"), QStringLiteral("Build Server")};

}  // namespace

TEST(TokenEdit, CommitsTypedTextOnReturn) {
    TokenEdit editor;
    editor.set_known_values(kKnown);
    QLineEdit* input = input_of(editor);
    ASSERT_NE(input, nullptr);

    QTest::keyClicks(input, QStringLiteral("Build Server"));
    QTest::keyClick(input, Qt::Key_Return);

    EXPECT_EQ(joined(editor.tokens()), "Build Server");
    EXPECT_TRUE(input->text().isEmpty()) << "the input should clear once the token is committed";
}

TEST(TokenEdit, CommitsOnAComma) {
    TokenEdit editor;
    editor.set_known_values(kKnown);

    QTest::keyClicks(input_of(editor), QStringLiteral("Build Server,"));

    EXPECT_EQ(joined(editor.tokens()), "Build Server");
}

TEST(TokenEdit, EmitsTokensChangedWhenTheUserCommits) {
    TokenEdit editor;
    editor.set_known_values(kKnown);
    QSignalSpy spy(&editor, &TokenEdit::tokens_changed);
    ASSERT_TRUE(spy.isValid());

    QTest::keyClicks(input_of(editor), QStringLiteral("Build Server"));
    QTest::keyClick(input_of(editor), Qt::Key_Return);

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(joined(spy.front().front().toStringList()), "Build Server");
}

TEST(TokenEdit, SettingTokensProgrammaticallyDoesNotEmit) {
    // set_tokens() is the caller pushing state in, not the user editing it.
    // Echoing it back would loop straight through whatever slot is listening.
    TokenEdit editor;
    QSignalSpy spy(&editor, &TokenEdit::tokens_changed);
    ASSERT_TRUE(spy.isValid());

    editor.set_tokens(kKnown);

    EXPECT_EQ(joined(editor.tokens()), "Lab Workstation | Build Server");
    EXPECT_EQ(spy.count(), 0);
}

TEST(TokenEdit, RejectsBlankAndDuplicateTokens) {
    TokenEdit editor;
    editor.set_known_values(kKnown);
    editor.set_tokens({QStringLiteral("Build Server")});

    QTest::keyClicks(input_of(editor), QStringLiteral("   "));
    QTest::keyClick(input_of(editor), Qt::Key_Return);
    // Case-insensitively: assignments name templates, and one host holding the
    // same template twice is a mistake however it was capitalised.
    QTest::keyClicks(input_of(editor), QStringLiteral("build server"));
    QTest::keyClick(input_of(editor), Qt::Key_Return);

    EXPECT_EQ(joined(editor.tokens()), "Build Server");
}

TEST(TokenEdit, RemovesTheLastTokenOnBackspaceWhenTheInputIsEmpty) {
    TokenEdit editor;
    editor.set_tokens(kKnown);

    QTest::keyClick(input_of(editor), Qt::Key_Backspace);

    EXPECT_EQ(joined(editor.tokens()), "Lab Workstation");
}

TEST(TokenEdit, RemovesATokenWhenItsChipButtonIsClicked) {
    TokenEdit editor;
    editor.set_tokens(kKnown);
    QSignalSpy spy(&editor, &TokenEdit::tokens_changed);

    QToolButton* remove = remove_button_for(editor, QStringLiteral("Lab Workstation"));
    ASSERT_NE(remove, nullptr) << "every chip needs its own remove button";
    remove->click();

    EXPECT_EQ(joined(editor.tokens()), "Build Server");
    EXPECT_EQ(spy.count(), 1) << "removing a token is a user edit and must be announced";
}

TEST(TokenEdit, FlagsTextThatMatchesNoKnownValueWhileItIsTyped) {
    // The flag drives the colour from the stylesheet rather than a hard-coded
    // one here, so the palette stays in theme.qss with every other colour.
    TokenEdit editor;
    editor.set_known_values(kKnown);
    QLineEdit* input = input_of(editor);

    QTest::keyClicks(input, QStringLiteral("Ren"));
    EXPECT_TRUE(input->property("unknown").toBool())
        << "a partly-typed name that matches nothing yet is still unknown";

    input->clear();
    QTest::keyClicks(input, QStringLiteral("build server"));
    EXPECT_FALSE(input->property("unknown").toBool())
        << "an existing template, whatever its capitalisation, is not new";

    input->clear();
    EXPECT_FALSE(input->property("unknown").toBool()) << "an empty input is not a new value";
}

TEST(TokenEdit, StopsFlaggingAValueOnceItBecomesKnown) {
    // The server creates a template the moment an unknown name is committed,
    // then hands the widget the longer list. The hint has to follow.
    TokenEdit editor;
    editor.set_known_values(kKnown);
    QLineEdit* input = input_of(editor);

    QTest::keyClicks(input, QStringLiteral("Renderfarm"));
    ASSERT_TRUE(input->property("unknown").toBool());

    editor.set_known_values(kKnown + QStringList{QStringLiteral("Renderfarm")});

    EXPECT_FALSE(input->property("unknown").toBool());
}

TEST(TokenEdit, ClearsTheInputWhenTheCommittedValueIsCreatedInResponse) {
    // The exact server round-trip: committing an unknown name makes the window
    // create the template and hand every editor the longer list back, from
    // inside this very signal.
    TokenEdit editor;
    editor.set_known_values(kKnown);
    QObject::connect(&editor, &TokenEdit::tokens_changed, &editor,
                      [&editor](const QStringList& tokens) {
                          editor.set_known_values(kKnown + tokens);
                      });

    QLineEdit* input = input_of(editor);
    QTest::keyClicks(input, QStringLiteral("Renderfarm"));
    QTest::keyClick(input, Qt::Key_Return);

    EXPECT_EQ(joined(editor.tokens()), "Renderfarm");
    EXPECT_TRUE(input->text().isEmpty())
        << "left in the input: " << input->text().toStdString();
}

TEST(TokenEdit, ClearsTheInputWhenLivingInATableCell) {
    // Faithful to the server: the editor is a cell widget in a shown table,
    // and the round-trip that creates the template runs from inside the signal.
    QTableWidget table(1, 2);
    auto* editor = new TokenEdit(&table);
    editor->set_known_values(kKnown);
    table.setCellWidget(0, 1, editor);
    table.resize(600, 120);
    table.show();
    QApplication::processEvents();

    QObject::connect(editor, &TokenEdit::tokens_changed, editor, [editor](const QStringList& tokens) {
        editor->set_known_values(kKnown + tokens);
    });

    QLineEdit* input = editor->findChild<QLineEdit*>();
    ASSERT_NE(input, nullptr);
    input->setFocus();
    QTest::keyClicks(input, QStringLiteral("Renderfarm"));
    QTest::keyClick(input, Qt::Key_Return);
    QApplication::processEvents();

    EXPECT_EQ(joined(editor->tokens()), "Renderfarm");
    EXPECT_TRUE(input->text().isEmpty())
        << "left in the input: " << input->text().toStdString();
}

TEST(TokenEdit, PaintsAnUnknownValueInTheWarningColour) {
    // The property test above proves the widget's half of this. Only painting
    // proves the other half -- that a stylesheet rule actually matches the
    // property, which is the join that silently fails.
    TokenEdit editor;
    editor.set_known_values(kKnown);
    editor.resize(320, 34);
    editor.show();
    QApplication::processEvents();

    QLineEdit* input = input_of(editor);
    QTest::keyClicks(input, QStringLiteral("Renderfarm"));
    QApplication::processEvents();

    EXPECT_TRUE(lm::ui::test::contains_colour(lm::ui::test::paint(*input), QColor(Theme::kOffline)))
        << "a name matching no template must be visibly yellow, not just flagged";
}

TEST(TokenEdit, PaintsAKnownValueInThePlainTextColour) {
    TokenEdit editor;
    editor.set_known_values(kKnown);
    editor.resize(320, 34);
    editor.show();
    QApplication::processEvents();

    QLineEdit* input = input_of(editor);
    QTest::keyClicks(input, QStringLiteral("Build Server"));
    QApplication::processEvents();

    const QImage painted = lm::ui::test::paint(*input);
    EXPECT_TRUE(lm::ui::test::contains_colour(painted, QColor(Theme::kText)));
    EXPECT_FALSE(lm::ui::test::contains_colour(painted, QColor(Theme::kOffline)))
        << "an existing template is not new and must not be marked as such";
}

TEST(TokenEdit, OffersEveryKnownValueThroughItsCompleter) {
    TokenEdit editor;
    editor.set_known_values(kKnown);

    QCompleter* completer = input_of(editor)->completer();
    ASSERT_NE(completer, nullptr) << "there is no dropdown without a completer";
    ASSERT_NE(completer->model(), nullptr);
    EXPECT_EQ(completer->model()->rowCount(), kKnown.size());
    EXPECT_EQ(completer->caseSensitivity(), Qt::CaseInsensitive);
}

